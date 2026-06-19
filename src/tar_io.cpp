// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

#include "tar_io.hpp"
#include "security.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <cstdlib>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace tapir
{

    // Match on the UTF-8 member name (locale-independent), tolerating a leading "./".
    static const char *epath(struct archive_entry *e)
    {
        const char *p = archive_entry_pathname_utf8(e);
        return p ? p : archive_entry_pathname(e);
    }
    static bool match(const char *p, const std::string &name)
    {
        if (!p)
            return false;
        if (name == p)
            return true;
        return p[0] == '.' && p[1] == '/' && name == (p + 2);
    }

    // ── offset-correcting reader ────────────────────────────────────────────────
    // A tape read returns one whole physical block, so libarchive cannot be started
    // mid-block by a short read. tar_open_at_block_offset reads the block that holds
    // a member's header in full, then feeds libarchive the bytes from the header
    // onward (skipping the leading `block_offset` bytes — the tail of the previous
    // member that shares the block) and continues reading whole blocks from the fd.
    namespace
    {
        struct BlockOffsetSource
        {
            int fd;                       // borrowed read fd, positioned at the block start
            std::vector<std::byte> block; // one physical block, reused for each read
            std::size_t skip = 0;         // header offset within the first block
            std::size_t first_len = 0;
            bool first = true;
        };

        la_ssize_t bo_read(struct archive *, void *cd, const void **buf)
        {
            auto *s = static_cast<BlockOffsetSource *>(cd);
            if (s->first) // serve the first block from `skip` onward (the header)
            {
                s->first = false;
                *buf = s->block.data() + s->skip;
                return static_cast<la_ssize_t>(s->first_len - s->skip);
            }
            const la_ssize_t n = ::read(s->fd, s->block.data(), s->block.size());
            if (n < 0)
                return -1;
            *buf = s->block.data();
            return n;
        }

        int bo_close(struct archive *, void *cd)
        {
            delete static_cast<BlockOffsetSource *>(cd);
            return ARCHIVE_OK;
        }
    } // namespace

    ArchiveReadPtr tar_open_at_block_offset(int fd, int bsize, int64_t block_offset)
    {
        if (fd < 0 || bsize <= 0 || block_offset < 0 || block_offset >= bsize)
            return nullptr;

        auto *s = new BlockOffsetSource;
        s->fd = fd;
        s->block.resize(static_cast<std::size_t>(bsize));
        s->skip = static_cast<std::size_t>(block_offset);

        const la_ssize_t n = ::read(fd, s->block.data(), s->block.size());
        if (n <= block_offset) // header offset beyond the bytes actually present
        {
            delete s;
            return nullptr;
        }
        s->first_len = static_cast<std::size_t>(n);

        ArchiveReadPtr a(archive_read_new());
        archive_read_support_filter_all(a.get());
        archive_read_support_format_all(a.get());
        // On failure, archive_read_free (via the ArchiveReadPtr deleter) invokes
        // bo_close, freeing `s` — so there is no leak on the error path either.
        if (archive_read_open2(a.get(), s, nullptr, bo_read, nullptr, bo_close) != ARCHIVE_OK)
            return nullptr;
        return a;
    }

    bool tar_read_member(struct archive *a, const std::string &member, std::string &out)
    {
        struct archive_entry *e;
        while (archive_read_next_header(a, &e) == ARCHIVE_OK)
        {
            if (match(epath(e), member))
            {
                out.clear();
                const void *b;
                size_t n;
                la_int64_t off;
                int r;
                while ((r = archive_read_data_block(a, &b, &n, &off)) == ARCHIVE_OK)
                    out.append(static_cast<const char *>(b), n);
                return r == ARCHIVE_EOF;
            }
            archive_read_data_skip(a);
        }
        return false;
    }

    bool tar_extract_member(struct archive *a, const std::string &member, Fd &out_fd, uint64_t &out_size)
    {
        struct archive_entry *e;
        while (archive_read_next_header(a, &e) == ARCHIVE_OK)
        {
            if (match(epath(e), member))
            {
                char tmpl[] = "/tmp/tapir-cacheXXXXXX";
                Fd fd(mkstemp(tmpl));
                if (!fd.valid())
                    return false;
                ::unlink(tmpl);
                const void *b;
                size_t n;
                la_int64_t off;
                int r;
                while ((r = archive_read_data_block(a, &b, &n, &off)) == ARCHIVE_OK)
                    if (pwrite(fd.get(), b, n, static_cast<off_t>(off)) != static_cast<ssize_t>(n))
                        return false;
                if (r != ARCHIVE_EOF)
                    return false;
                const la_int64_t sz = archive_entry_size(e);
                out_size = sz >= 0 ? static_cast<uint64_t>(sz) : 0;
                out_fd = std::move(fd);
                return true;
            }
            archive_read_data_skip(a);
        }
        return false;
    }

    bool tar_write_files(struct archive *a, const std::vector<OutFile> &files)
    {
        std::vector<char> buf(kReadBlock);
        for (const auto &f : files)
        {
            ArchiveEntryPtr e(archive_entry_new());
            archive_entry_set_pathname_utf8(e.get(), f.name.c_str());
            archive_entry_set_size(e.get(), static_cast<la_int64_t>(f.size));
            archive_entry_set_filetype(e.get(), AE_IFREG);
            archive_entry_set_perm(e.get(), f.mode ? f.mode : kFileMode);
            archive_entry_set_mtime(e.get(), f.mtime ? f.mtime : std::time(nullptr), 0);
            if (archive_write_header(a, e.get()) != ARCHIVE_OK)
                return false;
            off_t off = 0;
            uint64_t remaining = f.size;
            while (remaining > 0)
            {
                const size_t want = remaining < buf.size() ? static_cast<size_t>(remaining) : buf.size();
                const ssize_t got = pread(f.fd, buf.data(), want, off);
                if (got <= 0)
                    return false;
                if (archive_write_data(a, buf.data(), static_cast<size_t>(got)) < 0)
                    return false;
                off += got;
                remaining -= static_cast<uint64_t>(got);
            }
        }
        return true;
    }

    bool tar_write_member(struct archive *a, const std::string &member, const std::string &data)
    {
        ArchiveEntryPtr e(archive_entry_new());
        archive_entry_set_pathname(e.get(), member.c_str());
        archive_entry_set_size(e.get(), static_cast<la_int64_t>(data.size()));
        archive_entry_set_filetype(e.get(), AE_IFREG);
        archive_entry_set_perm(e.get(), kFileMode);
        archive_entry_xattr_add_entry(e.get(), kTapirMagicXattrName,
                                      kTapirMagicXattrValue,
                                      std::strlen(kTapirMagicXattrValue));
        if (archive_write_header(a, e.get()) != ARCHIVE_OK)
            return false;
        if (!data.empty() && archive_write_data(a, data.data(), data.size()) < 0)
            return false;
        return true;
    }

    bool tar_entry_has_tapir_magic(struct archive_entry *e)
    {
        archive_entry_xattr_reset(e);
        const char *xname;
        const void *xval;
        size_t xsz;
        while (archive_entry_xattr_next(e, &xname, &xval, &xsz) == ARCHIVE_OK)
        {
            if (std::strcmp(xname, kTapirMagicXattrName) == 0 &&
                xsz == std::strlen(kTapirMagicXattrValue) &&
                std::memcmp(xval, kTapirMagicXattrValue, xsz) == 0)
                return true;
        }
        return false;
    }

    static std::string normalize(const char *p)
    {
        std::string s = p ? p : "";
        while (s.rfind("./", 0) == 0)
            s.erase(0, 2); // strip leading "./"
        while (!s.empty() && s[0] == '/')
            s.erase(0, 1); // strip leading "/"
        return s;
    }

    bool tar_copy_members(
        struct archive *in, struct archive *out,
        const std::function<void(const std::string &, const std::string &, uint64_t, time_t, mode_t)> &cb)
    {
        struct archive_entry *e;
        int r;
        while ((r = archive_read_next_header(in, &e)) == ARCHIVE_OK)
        {
            const std::string name = normalize(epath(e));
            const time_t entry_mtime = archive_entry_mtime(e);
            const mode_t entry_mode = archive_entry_perm(e);
            archive_entry_set_pathname_utf8(e, name.c_str()); // normalise on-tape name too
            if (archive_write_header(out, e) != ARCHIVE_OK)
                return false;
            const bool is_file = archive_entry_filetype(e) == AE_IFREG && archive_entry_hardlink(e) == nullptr;
            security::Sha256 sha;
            uint64_t total = 0;
            const void *b;
            size_t n;
            la_int64_t off;
            int rr;
            while ((rr = archive_read_data_block(in, &b, &n, &off)) == ARCHIVE_OK)
            {
                if (archive_write_data(out, b, n) < 0)
                    return false;
                if (is_file)
                {
                    sha.update(b, n);
                    total += n;
                }
            }
            if (rr != ARCHIVE_EOF)
                return false;
            if (is_file)
                cb(name, sha.hex(), total, entry_mtime, entry_mode);
        }
        return r == ARCHIVE_EOF;
    }

    bool tar_for_each_member(
        struct archive *a,
        const std::function<void(const std::string &, const std::string &, uint64_t, time_t, mode_t)> &cb,
        const std::function<void(const std::string &, bool)> &on_header)
    {
        struct archive_entry *e;
        int r;
        while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK)
        {
            if (archive_entry_filetype(e) != AE_IFREG)
            {
                archive_read_data_skip(a);
                continue;
            }
            const std::string name = normalize(epath(e));
            const time_t entry_mtime = archive_entry_mtime(e);
            const mode_t entry_mode = archive_entry_perm(e);
            const bool is_tapir_index = (name == "manifest.json") && tar_entry_has_tapir_magic(e);
            if (on_header)
                on_header(name, is_tapir_index);
            security::Sha256 sha;
            const void *b;
            size_t n;
            la_int64_t off;
            int rr;
            uint64_t total = 0;
            while ((rr = archive_read_data_block(a, &b, &n, &off)) == ARCHIVE_OK)
            {
                sha.update(b, n);
                total += n;
            }
            if (rr != ARCHIVE_EOF)
                return false;
            cb(name, sha.hex(), total, entry_mtime, entry_mode);
        }
        return r == ARCHIVE_EOF;
    }

    bool tar_for_each_member_with_blocks(
        struct archive *a, int64_t bsize,
        const std::function<void(const std::string &, int64_t, const std::string &,
                                 uint64_t, time_t, mode_t)> &cb,
        const std::function<void(const std::string &, int64_t, bool)> &on_header)
    {
        struct archive_entry *e;
        int r;
        while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK)
        {
            if (archive_entry_filetype(e) != AE_IFREG)
            {
                archive_read_data_skip(a);
                continue;
            }
            // Capture physical-block offset immediately after the header is read.
            // archive_filter_bytes is always a multiple of bsize (libarchive reads
            // one bsize-byte block per syscall). The header is in the most recently
            // read block, which is the one just before the current read position:
            //   block = (filter_bytes - 1) / bsize   (integer division)
            // This is the same value the writer stores via archive_filter_bytes/bsize
            // captured before writing each member.
            const la_int64_t raw = archive_filter_bytes(a, -1);
            const int64_t block = (bsize > 0 && raw > 0) ? (raw - 1) / bsize : 0;

            const std::string name = normalize(epath(e));
            const time_t entry_mtime = archive_entry_mtime(e);
            const mode_t entry_mode = archive_entry_perm(e);
            const bool is_tapir_index = (name == "manifest.json") && tar_entry_has_tapir_magic(e);
            if (on_header)
                on_header(name, block, is_tapir_index);
            security::Sha256 sha;
            const void *b;
            size_t n;
            la_int64_t off;
            int rr;
            uint64_t total = 0;
            while ((rr = archive_read_data_block(a, &b, &n, &off)) == ARCHIVE_OK)
            {
                sha.update(b, n);
                total += n;
            }
            if (rr != ARCHIVE_EOF)
                return false;
            cb(name, block, sha.hex(), total, entry_mtime, entry_mode);
        }
        return r == ARCHIVE_EOF;
    }

} // namespace tapir
