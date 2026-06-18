// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

#include "tape.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <cstdio>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mtio.h>
#include <unistd.h>

namespace tapir
{

    // All positioning goes through the Linux st driver's ioctl interface
    // (<sys/mtio.h>) — the same calls the `mt` tool wraps. No subprocess, no
    // dependency on the mt binary, and the file number comes straight from MTIOCGET.
    bool Tape::ctl(short op, int count)
    {
        Fd fd(::open(dev_.c_str(), O_RDONLY)); // opening the no-rewind node does not reposition
        if (!fd.valid())
            return false;
        struct mtop mt{op, count};
        return ioctl(fd.get(), MTIOCTOP, &mt) == 0;
    }

    bool Tape::rewind()
    {
        if (verbose_)
            std::fprintf(stderr, "  tape: rewind to start of tape\n");
        return ctl(MTREW, 1);
    }
    bool Tape::eod()
    {
        if (verbose_)
            std::fprintf(stderr, "  tape: space to end-of-data\n");
        return ctl(MTEOM, 1); // the append point
    }
    bool Tape::fsf(int n)
    {
        if (verbose_)
            std::fprintf(stderr, "  tape: forward over %d file mark(s)\n", n);
        return ctl(MTFSF, n);
    }
    bool Tape::bsf(int n)
    {
        if (verbose_)
            std::fprintf(stderr, "  tape: back over %d file mark(s)\n", n);
        return ctl(MTBSF, n);
    }

    int Tape::file_number()
    {
        Fd fd(::open(dev_.c_str(), O_RDONLY));
        if (!fd.valid())
            return -1;
        struct mtget g{};
        if (ioctl(fd.get(), MTIOCGET, &g) != 0)
            return -1;
        const int f = g.mt_fileno >= 0 ? static_cast<int>(g.mt_fileno) : 0;
        if (verbose_)
            std::fprintf(stderr, "  tape: now at file %d\n", f);
        return f;
    }

    // ── positioning ───────────────────────────────────────────────────────────────
    bool Tape::position_data(int tape_file)
    {
        if (verbose_)
            std::fprintf(stderr, "tape: positioning to data tape file %d\n", tape_file);
        if (!rewind())
            return false;
        return tape_file > 0 ? fsf(tape_file) : true;
    }

    // Latest manifest is the last tape file. From EOD, the manifest sits one file
    // back: cross 2 filemarks back, then forward 1 to land on its first block.
    bool Tape::position_latest_manifest()
    {
        if (!eod())
            return false;
        const int n = file_number();
        if (n <= 0)
            return false;
        const int target = n - 1;
        if (target == 0)
            return rewind();
        return bsf(2) && fsf(1);
    }

    int Tape::survey(bool &full)
    {
        full = false;
        if (!eod()) // one fast locate to end-of-data
            return -1;
        Fd fd(::open(dev_.c_str(), O_RDONLY));
        if (!fd.valid())
            return -1;
        struct mtget g{};
        if (ioctl(fd.get(), MTIOCGET, &g) != 0)
            return -1;
        full = GMT_EOT(g.mt_gstat) != 0; // at physical end of tape → no room for an index
        const int count = g.mt_fileno >= 0 ? static_cast<int>(g.mt_fileno) : 0;
        if (verbose_)
            std::fprintf(stderr, "  tape: end-of-data — %d file(s) on tape%s\n",
                         count, full ? ", tape is FULL" : "");
        return count;
    }

    // ── reads ─────────────────────────────────────────────────────────────────────
    static ArchiveReadPtr open_read(const std::string &dev, int bsize, Fd &fd_holder)
    {
        Fd fd(::open(dev.c_str(), O_RDONLY));
        if (!fd.valid())
            return nullptr;
        ArchiveReadPtr a(archive_read_new());
        archive_read_support_filter_all(a.get());  // gzip/xz/zstd/etc.
        archive_read_support_format_all(a.get()); // V7/ustar/GNU/pax/star — all tar variants
        if (archive_read_open_fd(a.get(), fd.get(), static_cast<size_t>(bsize)) != ARCHIVE_OK)
            return nullptr;
        fd_holder = std::move(fd); // fd must outlive the archive
        return a;
    }

    // Read manifest.json from the tape's current position. Checks that the first
    // (and only) member is manifest.json; rejects data tape files without
    // scanning them.
    static bool read_manifest_body(const std::string &dev, int mbf, std::string &out)
    {
        Fd fd;
        ArchiveReadPtr a = open_read(dev, mbf * 512, fd);
        if (!a)
            return false;
        struct archive_entry *e;
        if (archive_read_next_header(a.get(), &e) != ARCHIVE_OK)
            return false;
        const char *p = archive_entry_pathname_utf8(e);
        if (!p)
            p = archive_entry_pathname(e);
        if (!p || std::string(p) != "manifest.json")
            return false;
        out.clear();
        const void *b;
        size_t n;
        la_int64_t off;
        int r;
        while ((r = archive_read_data_block(a.get(), &b, &n, &off)) == ARCHIVE_OK)
            out.append(static_cast<const char *>(b), n);
        return r == ARCHIVE_EOF;
    }

    bool Tape::read_latest_manifest(std::string &out)
    {
        if (!position_latest_manifest())
            return false;
        return read_manifest_body(dev_, mbf_, out);
    }

    bool Tape::read_manifest_at(int tape_file, std::string &out)
    {
        if (!position_data(tape_file))
            return false;
        return read_manifest_body(dev_, mbf_, out);
    }

    bool Tape::read_member(int tape_file, int block_factor, const std::string &member,
                           Fd &out_fd, uint64_t &out_size)
    {
        if (!position_data(tape_file))
            return false;
        Fd fd;
        ArchiveReadPtr a = open_read(dev_, block_factor * 512, fd);
        if (!a)
            return false;
        return tar_extract_member(a.get(), member, out_fd, out_size);
    }

    bool Tape::scan_archive(int tape_file, int block_factor,
                            const std::function<void(const std::string &, const std::string &, uint64_t, time_t)> &cb,
                            const std::function<void(const std::string &)> &on_header)
    {
        if (!position_data(tape_file))
            return false;
        Fd fd;
        ArchiveReadPtr a = open_read(dev_, block_factor * 512, fd);
        if (!a)
            return false;
        return tar_for_each_member(a.get(), cb, on_header);
    }

    bool Tape::scan_archive_detect(int tape_file, int &detected,
                                   const std::function<void(const std::string &, const std::string &, uint64_t, time_t)> &cb,
                                   const std::function<void(const std::string &)> &on_header)
    {
        detected = 0;

        // Pass 1: read one physical block to learn its size (in variable-block mode
        // a single read returns exactly one block).
        if (!position_data(tape_file))
            return false;
        {
            Fd fd(::open(dev_.c_str(), O_RDONLY));
            if (!fd.valid())
                return false;
            std::vector<char> buf(1u << 22); // 4 MiB: larger than any expected tape block
            const ssize_t n = ::read(fd.get(), buf.data(), buf.size());
            if (n <= 0)
            {
                if (verbose_)
                    std::fprintf(stderr, "  tape: file %d is empty or unreadable\n", tape_file);
                return false;
            }
            detected = static_cast<int>(n);
        }
        if (verbose_)
            std::fprintf(stderr, "  tape: file %d physical block size = %d bytes\n", tape_file, detected);

        // Pass 2: rewind and scan with the proven open_fd path, using a read buffer
        // sized to the detected block so reads never come up short.
        if (!position_data(tape_file))
            return false;
        Fd fd;
        ArchiveReadPtr a = open_read(dev_, detected, fd);
        if (!a)
        {
            if (verbose_)
                std::fprintf(stderr, "  tape: file %d: could not open for reading\n", tape_file);
            return false;
        }
        const bool ok = tar_for_each_member(a.get(), cb, on_header);
        if (!ok && verbose_)
            std::fprintf(stderr, "  tape: file %d: libarchive: %s\n",
                         tape_file, archive_error_string(a.get()));
        return ok;
    }

    // ── writes ────────────────────────────────────────────────────────────────────
    bool Tape::write_tape_file(int block_factor, const std::function<bool(struct archive *)> &writer)
    {
        const int bsize = block_factor * 512;
        Fd fd(::open(dev_.c_str(), O_WRONLY));
        if (!fd.valid())
            return false;
        ArchiveWritePtr a(archive_write_new());
        archive_write_set_format_pax_restricted(a.get());
        archive_write_set_bytes_per_block(a.get(), bsize);
        archive_write_set_bytes_in_last_block(a.get(), bsize); // pad final block (tape needs full blocks)
        if (archive_write_open_fd(a.get(), fd.get()) != ARCHIVE_OK)
            return false;
        if (!writer(a.get()))
            return false;
        return archive_write_close(a.get()) == ARCHIVE_OK; // fd closes after → st writes a filemark
    }

    bool Tape::append(int block_factor, const std::function<bool(struct archive *)> &write_data,
                      const std::function<std::string(int)> &make_manifest, int &out_dtf)
    {
        if (!eod())
            return false;
        const int dtf = file_number();
        if (dtf < 0)
            return false;
        out_dtf = dtf;

        if (write_data)
        { // data first (skipped if null)…
            if (!write_tape_file(block_factor, write_data))
                return false;
        }
        const std::string manifest = make_manifest(dtf);
        return write_tape_file(mbf_, // …then the index
                               [&](struct archive *a)
                               { return tar_write_member(a, "manifest.json", manifest); });
    }

    bool Tape::write_data_at_eod(int block_factor,
                                const std::function<bool(struct archive *)> &writer,
                                int &out_tape_file)
    {
        if (!eod())
            return false;
        out_tape_file = file_number();
        if (out_tape_file < 0)
            return false;
        return write_tape_file(block_factor, writer);
    }

    bool Tape::write_manifest_at_eod(const std::string &manifest_json, int &out_tape_file)
    {
        if (!eod())
            return false;
        out_tape_file = file_number();
        if (out_tape_file < 0)
            return false;
        return write_tape_file(mbf_,
                               [&manifest_json](struct archive *a)
                               { return tar_write_member(a, "manifest.json", manifest_json); });
    }

    bool Tape::overwrite_from_start(int block_factor,
                                    const std::function<std::string()> &make_manifest)
    {
        if (!rewind())
            return false;
        const std::string manifest = make_manifest();
        return write_tape_file(block_factor,
                               [&](struct archive *a)
                               { return tar_write_member(a, "manifest.json", manifest); });
    }

} // namespace tapir
