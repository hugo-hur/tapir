#include "tape.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mtio.h>
#include <unistd.h>

namespace tapir {

// All positioning goes through the Linux st driver's ioctl interface
// (<sys/mtio.h>) — the same calls the `mt` tool wraps. No subprocess, no
// dependency on the mt binary, and the file number comes straight from MTIOCGET.
bool Tape::ctl(short op, int count) {
    Fd fd(::open(dev_.c_str(), O_RDONLY));   // opening the no-rewind node does not reposition
    if (!fd.valid()) return false;
    struct mtop mt{op, count};
    return ioctl(fd.get(), MTIOCTOP, &mt) == 0;
}

bool Tape::rewind()   { return ctl(MTREW, 1); }
bool Tape::eod()      { return ctl(MTEOM, 1); }   // space to end of data (the append point)
bool Tape::fsf(int n) { return ctl(MTFSF, n); }
bool Tape::bsf(int n) { return ctl(MTBSF, n); }

int Tape::file_number() {
    Fd fd(::open(dev_.c_str(), O_RDONLY));
    if (!fd.valid()) return -1;
    struct mtget g{};
    if (ioctl(fd.get(), MTIOCGET, &g) != 0) return -1;
    return g.mt_fileno >= 0 ? static_cast<int>(g.mt_fileno) : 0;
}

// ── positioning ───────────────────────────────────────────────────────────────
bool Tape::position_data(int tape_file) {
    if (!rewind()) return false;
    return tape_file > 0 ? fsf(tape_file) : true;
}

// Latest manifest is the last tape file. From EOD, the manifest sits one file
// back: cross 2 filemarks back, then forward 1 to land on its first block.
bool Tape::position_latest_manifest() {
    if (!eod()) return false;
    const int n = file_number();
    if (n <= 0) return false;
    const int target = n - 1;
    if (target == 0) return rewind();
    return bsf(2) && fsf(1);
}

int Tape::probe_block_size(int tape_file) {
    if (!position_data(tape_file)) return -1;
    Fd fd(::open(dev_.c_str(), O_RDONLY));
    if (!fd.valid()) return -1;
    std::vector<char> buf(1u << 22);          // 4 MiB: larger than any expected tape block
    const ssize_t n = ::read(fd.get(), buf.data(), buf.size());  // one read → one physical block
    return n > 0 ? static_cast<int>(n) : -1;
}

// ── reads ─────────────────────────────────────────────────────────────────────
static ArchiveReadPtr open_read(const std::string& dev, int bsize, Fd& fd_holder) {
    Fd fd(::open(dev.c_str(), O_RDONLY));
    if (!fd.valid()) return nullptr;
    ArchiveReadPtr a(archive_read_new());
    archive_read_support_format_tar(a.get());
    if (archive_read_open_fd(a.get(), fd.get(), static_cast<size_t>(bsize)) != ARCHIVE_OK)
        return nullptr;
    fd_holder = std::move(fd);     // fd must outlive the archive
    return a;
}

bool Tape::read_latest_manifest(std::string& out) {
    if (!position_latest_manifest()) return false;
    Fd fd;
    ArchiveReadPtr a = open_read(dev_, mbf_ * 512, fd);
    if (!a) return false;
    return tar_read_member(a.get(), "manifest.json", out);
}

bool Tape::read_member(int tape_file, int block_factor, const std::string& member,
                       Fd& out_fd, uint64_t& out_size) {
    if (!position_data(tape_file)) return false;
    Fd fd;
    ArchiveReadPtr a = open_read(dev_, block_factor * 512, fd);
    if (!a) return false;
    return tar_extract_member(a.get(), member, out_fd, out_size);
}

bool Tape::scan_archive(int tape_file, int block_factor,
                        const std::function<void(const std::string&, const std::string&, uint64_t)>& cb) {
    if (!position_data(tape_file)) return false;
    Fd fd;
    ArchiveReadPtr a = open_read(dev_, block_factor * 512, fd);
    if (!a) return false;
    return tar_for_each_member(a.get(), cb);
}

// ── writes ────────────────────────────────────────────────────────────────────
bool Tape::write_tape_file(int block_factor, const std::function<bool(struct archive*)>& writer) {
    const int bsize = block_factor * 512;
    Fd fd(::open(dev_.c_str(), O_WRONLY));
    if (!fd.valid()) return false;
    ArchiveWritePtr a(archive_write_new());
    archive_write_set_format_pax_restricted(a.get());
    archive_write_set_bytes_per_block(a.get(), bsize);
    archive_write_set_bytes_in_last_block(a.get(), bsize);   // pad final block (tape needs full blocks)
    if (archive_write_open_fd(a.get(), fd.get()) != ARCHIVE_OK) return false;
    if (!writer(a.get())) return false;
    return archive_write_close(a.get()) == ARCHIVE_OK;       // fd closes after → st writes a filemark
}

bool Tape::append(int block_factor, const std::function<bool(struct archive*)>& write_data,
                  const std::function<std::string(int)>& make_manifest, int& out_dtf) {
    if (!eod()) return false;
    const int dtf = file_number();
    if (dtf < 0) return false;
    out_dtf = dtf;

    if (write_data) {                                        // data first (skipped if null)…
        if (!write_tape_file(block_factor, write_data)) return false;
    }
    const std::string manifest = make_manifest(dtf);
    return write_tape_file(mbf_,                              // …then the index
                           [&](struct archive* a) { return tar_write_member(a, "manifest.json", manifest); });
}

} // namespace tapir
