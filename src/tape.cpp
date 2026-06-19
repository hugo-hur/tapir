// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

#include "tape.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <cstdio>
#include <vector>

#include <cinttypes>
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
        if (!ctl(MTREW, 1)) return false;
        current_file_ = 0;
        return true;
    }
    bool Tape::eod()
    {
        if (verbose_)
            std::fprintf(stderr, "  tape: space to end-of-data\n");
        if (!ctl(MTEOM, 1)) return false;
        current_file_ = -1; // exact position unknown until file_number() is called
        return true;
    }
    bool Tape::fsf(int n)
    {
        if (verbose_)
            std::fprintf(stderr, "  tape: forward over %d file mark(s)\n", n);
        if (!ctl(MTFSF, n)) return false;
        if (current_file_ >= 0) current_file_ += n;
        return true;
    }
    bool Tape::bsf(int n)
    {
        if (verbose_)
            std::fprintf(stderr, "  tape: back over %d file mark(s)\n", n);
        if (!ctl(MTBSF, n)) return false;
        if (current_file_ >= 0) current_file_ -= n;
        return true;
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
        current_file_ = f; // update cached position
        return f;
    }

    int64_t Tape::block_number()
    {
        // MTIOCGET.mt_blkno is relative (within the current tape file) and is
        // reset to 0 after each file mark — useless for absolute positioning.
        // MTIOCPOS forces a SCSI READ POSITION command which returns the true
        // absolute Logical Object Identifier (LOID) needed by MTSEEK / Locate(16).
        // On drives that do not support READ POSITION (e.g. older LTO-1 hardware),
        // the ioctl fails and we return -1; callers fall back to rewind+fsf.
        Fd fd(::open(dev_.c_str(), O_RDONLY));
        if (!fd.valid())
            return -1;
        struct mtpos pos{};
        if (ioctl(fd.get(), MTIOCPOS, &pos) != 0)
            return -1;
        const int64_t b = static_cast<int64_t>(pos.mt_blkno);
        if (verbose_)
            std::fprintf(stderr, "  tape: absolute block %" PRId64 "\n", b);
        return b;
    }

    // ── positioning ───────────────────────────────────────────────────────────────
    bool Tape::position_data(int tape_file)
    {
        if (current_file_ >= 0 && current_file_ != tape_file)
        {
            const int delta = tape_file - current_file_;
            if (delta > 0)
            {
                // Target is ahead: forward-space, no rewind needed.
                if (verbose_)
                    std::fprintf(stderr, "tape: fsf %d to reach tape file %d (currently at %d)\n",
                                 delta, tape_file, current_file_);
                return fsf(delta);
            }
            else
            {
                // Target is behind: bsf(|delta|+1) + fsf(1) without a full rewind.
                // bsf(N+1) crosses N+1 filemarks backward, landing just before the
                // filemark preceding tape_file; fsf(1) steps past it to file start.
                const int back = -delta + 1;
                if (verbose_)
                    std::fprintf(stderr, "tape: bsf %d + fsf 1 to reach tape file %d (currently at %d)\n",
                                 back, tape_file, current_file_);
                return bsf(back) && fsf(1);
            }
        }
        if (current_file_ == tape_file)
            return true;

        // Position unknown: fall back to full rewind + forward-space.
        if (verbose_)
            std::fprintf(stderr, "tape: rewind + fsf %d to reach tape file %d (position unknown)\n",
                         tape_file, tape_file);
        if (!rewind())
            return false;
        return tape_file > 0 ? fsf(tape_file) : true;
    }

    bool Tape::fsr(int64_t n)
    {
        if (verbose_)
            std::fprintf(stderr, "  tape: fsr %" PRId64 " record(s) within tape file\n", n);
        return ctl(MTFSR, static_cast<int>(n));
    }

    bool Tape::seek_to(int tape_file, int64_t block_within_file)
    {
        if (!position_data(tape_file))
            return false;
        if (block_within_file > 0)
        {
            if (verbose_)
                std::fprintf(stderr, "tape: fsr %" PRId64 " to member within tape file %d\n",
                             block_within_file, tape_file);
            if (!fsr(block_within_file))
                return false;
        }
        return true;
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

    // Read manifest.json from the tape's current position. When require_magic is true
    // (normal operation) the entry must also carry the tapir PAX magic xattr; when
    // false (legacy/upgrade path) filename match alone is sufficient.
    static bool read_manifest_body(const std::string &dev, int mbf, std::string &out,
                                   bool require_magic)
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
        if (require_magic && !tar_entry_has_tapir_magic(e))
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
        const bool ok = read_manifest_body(dev_, mbf_, out, true);
        // Query actual post-read position from the driver. The st driver crosses the
        // filemark on fd close, so we land at the start of the next tape file; that
        // value lets position_data stream forward without rewinding.
        file_number();
        return ok;
    }

    bool Tape::read_latest_manifest_legacy(std::string &out)
    {
        if (!position_latest_manifest())
            return false;
        const bool ok = read_manifest_body(dev_, mbf_, out, false);
        file_number();
        return ok;
    }

    bool Tape::read_manifest_at(int tape_file, std::string &out)
    {
        if (!position_data(tape_file))
            return false;
        const bool ok = read_manifest_body(dev_, mbf_, out, false); // no magic req — admin op
        file_number(); // capture post-read position (see read_latest_manifest)
        return ok;
    }

    bool Tape::read_member(int tape_file, int block_factor, int64_t block_num, int64_t block_offset,
                           const std::string &member, Fd &out_fd, uint64_t &out_size)
    {
        const int bsize = block_factor * 512;

        // Fast path: position to the member's block, read that block in full, and
        // hand libarchive the bytes from the header offset onward (the bytes in
        // front belong to the previous member that shares this block).
        if (block_num >= 0 && block_offset >= 0)
        {
            if (seek_to(tape_file, block_num))
            {
                Fd fd(::open(dev_.c_str(), O_RDONLY));
                if (fd.valid())
                {
                    ArchiveReadPtr a = tar_open_at_block_offset(fd.get(), bsize, block_offset);
                    if (a && tar_extract_member(a.get(), member, out_fd, out_size))
                    {
                        current_file_ = -1; // mid-file after extract
                        return true;
                    }
                }
            }
            current_file_ = -1; // fast read failed — reposition cleanly for the fallback
        }

        // Fallback: scan the whole tape file from its start. Correct for any member,
        // and for archives whose per-member offset was never recorded.
        if (!position_data(tape_file))
            return false;
        Fd fd;
        ArchiveReadPtr a = open_read(dev_, bsize, fd);
        if (!a)
            return false;
        const bool ok = tar_extract_member(a.get(), member, out_fd, out_size);
        current_file_ = -1;
        return ok;
    }

    bool Tape::scan_archive(int tape_file, int block_factor, int64_t block_num,
                            const std::function<void(const std::string &, const std::string &,
                                                     uint64_t, time_t, mode_t)> &cb,
                            const std::function<void(const std::string &, bool)> &on_header)
    {
        if (!seek_to(tape_file, block_num))
            return false;
        Fd fd;
        ArchiveReadPtr a = open_read(dev_, block_factor * 512, fd);
        if (!a)
            return false;
        const bool ok = tar_for_each_member(a.get(), cb, on_header);
        file_number(); // capture post-read position (see read_latest_manifest)
        return ok;
    }

    bool Tape::scan_archive_with_blocks(int tape_file, int block_factor,
                                        const std::function<void(const std::string &, int64_t, int64_t,
                                                                 const std::string &, uint64_t,
                                                                 time_t, mode_t)> &cb,
                                        const std::function<void(const std::string &, int64_t, bool)> &on_header)
    {
        if (!position_data(tape_file))
            return false;
        Fd fd;
        ArchiveReadPtr a = open_read(dev_, block_factor * 512, fd);
        if (!a)
            return false;
        const bool ok = tar_for_each_member_with_blocks(
            a.get(), static_cast<int64_t>(block_factor) * 512, cb, on_header);
        file_number(); // capture post-read position (see read_latest_manifest)
        return ok;
    }

    bool Tape::scan_archive_detect(int tape_file, int &detected,
                                   const std::function<void(const std::string &, const std::string &,
                                                            uint64_t, time_t, mode_t)> &cb,
                                   const std::function<void(const std::string &, bool)> &on_header)
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
        // Raw read left us mid-file; current_file_ still says tape_file but we are
        // no longer at its start. Invalidate so position_data does a proper reposition.
        current_file_ = -1;
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
        file_number(); // capture post-read position (see read_latest_manifest)
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

    bool Tape::open_write_at_eod(int block_factor,
                                ArchiveWritePtr &out_ar, Fd &out_fd, int &out_tape_file)
    {
        if (!eod())
            return false;
        out_tape_file = file_number(); // also updates current_file_
        if (out_tape_file < 0)
            return false;

        const int bsize = block_factor * 512;
        Fd fd(::open(dev_.c_str(), O_WRONLY));
        if (!fd.valid())
            return false;

        ArchiveWritePtr a(archive_write_new());
        archive_write_set_format_pax_restricted(a.get());
        archive_write_set_bytes_per_block(a.get(), bsize);
        archive_write_set_bytes_in_last_block(a.get(), bsize);
        if (archive_write_open_fd(a.get(), fd.get()) != ARCHIVE_OK)
            return false;

        if (verbose_)
            std::fprintf(stderr, "  tape: opened tape file %d for streaming writes\n", out_tape_file);

        out_ar = std::move(a);
        out_fd = std::move(fd);
        return true;
    }

    void Tape::note_write_done(int tape_file)
    {
        current_file_ = tape_file + 1;
    }

    bool Tape::write_manifest_at_eod(const std::string &manifest_json, int &out_tape_file)
    {
        if (!eod())
            return false;
        out_tape_file = file_number(); // also updates current_file_
        if (out_tape_file < 0)
            return false;
        if (!write_tape_file(mbf_,
                             [&manifest_json](struct archive *a)
                             { return tar_write_member(a, "manifest.json", manifest_json); }))
            return false;
        current_file_ = out_tape_file + 1;
        return true;
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
