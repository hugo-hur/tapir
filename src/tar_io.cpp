// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

#include "tar_io.hpp"
#include "security.hpp"

#include <config.h>
#include <archive.h>
#include <archive_entry.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace tapir
{

    // Split a byte position into (block_index, within-block offset) given a physical
    // block size. Returns {-1, -1} when either input is non-positive / invalid.
    static inline void split_block_pos(la_int64_t pos, int64_t bsize,
                                       int64_t &out_block, int64_t &out_offset)
    {
        if (bsize > 0 && pos >= 0) { out_block = pos / bsize; out_offset = pos % bsize; }
        else                       { out_block = -1;           out_offset = -1; }
    }

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

    // Shared extraction body: finds the first header matching *member (or any header
    // when member is nullptr), writes data to an anonymous temp file, returns it.
    static bool extract_impl(struct archive *a, const std::string *member,
                             Fd &out_fd, uint64_t &out_size,
                             la_int64_t *out_header_pos)
    {
        struct archive_entry *e;
        while (archive_read_next_header(a, &e) == ARCHIVE_OK)
        {
            if (member && !match(epath(e), *member))
            {
                archive_read_data_skip(a);
                continue;
            }
            if (out_header_pos)
                *out_header_pos = archive_read_header_position(a);
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
        return false;
    }

    bool tar_extract_member(struct archive *a, const std::string &member, Fd &out_fd, uint64_t &out_size,
                            la_int64_t *out_header_pos)
    {
        return extract_impl(a, &member, out_fd, out_size, out_header_pos);
    }

    bool tar_extract_first_member(struct archive *a, Fd &out_fd, uint64_t &out_size)
    {
        return extract_impl(a, nullptr, out_fd, out_size, nullptr);
    }

    bool tar_write_file(struct archive *a, const OutFile &f,
                        int64_t bsize, int64_t &out_block, int64_t &out_offset)
    {
        ArchiveEntryPtr e(archive_entry_new());
        archive_entry_set_pathname_utf8(e.get(), f.name.c_str());
        archive_entry_set_size(e.get(), static_cast<la_int64_t>(f.size));
        archive_entry_set_filetype(e.get(), AE_IFREG);
        archive_entry_set_perm(e.get(), f.mode ? f.mode : kFileMode);
        archive_entry_set_mtime(e.get(), f.mtime ? f.mtime : std::time(nullptr), 0);
        if (archive_write_header(a, e.get()) != ARCHIVE_OK)
            return false;
#ifdef HAVE_ARCHIVE_WRITE_HEADER_POSITION
        const la_int64_t pos = archive_write_header_position(a);
        split_block_pos(pos, bsize, out_block, out_offset);
#else
        (void)bsize;
        out_block = out_offset = -1;
#endif
        std::vector<std::byte> buf(kReadBlock);
        off_t off = 0;
        uint64_t remaining = f.size;
        while (remaining > 0)
        {
            const size_t want = remaining < buf.size() ? static_cast<size_t>(remaining) : buf.size();
            const ssize_t got = pread(f.fd, static_cast<void *>(buf.data()), want, off);
            if (got <= 0)
                return false;
            if (archive_write_data(a, buf.data(), static_cast<size_t>(got)) < 0)
                return false;
            off += got;
            remaining -= static_cast<uint64_t>(got);
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

    bool tar_copy_members_with_blocks(
        struct archive *in, struct archive *out, int64_t bsize,
        const std::function<void(const std::string &, int64_t, int64_t,
                                 const std::string &, uint64_t, time_t, mode_t)> &cb)
    {
#ifndef HAVE_ARCHIVE_WRITE_HEADER_POSITION
        (void)bsize;
        // Fall back to tar_copy_members with a stub that passes block=-1, offset=-1.
        return tar_copy_members(in, out,
            [&](const std::string &name, const std::string &sha, uint64_t size, time_t mt, mode_t mo)
            { cb(name, -1, -1, sha, size, mt, mo); });
#else
        struct archive_entry *e;
        int r;
        while ((r = archive_read_next_header(in, &e)) == ARCHIVE_OK)
        {
            const std::string name = normalize(epath(e));
            const time_t entry_mtime = archive_entry_mtime(e);
            const mode_t entry_mode = archive_entry_perm(e);
            archive_entry_set_pathname_utf8(e, name.c_str());
            if (archive_write_header(out, e) != ARCHIVE_OK)
                return false;
            // Capture the byte offset of this header in the write stream immediately
            // after archive_write_header() — this is what archive_write_header_position()
            // was added to the tapir fork to provide.
            const la_int64_t pos = archive_write_header_position(out);
            int64_t block = -1, offset = -1;
            split_block_pos(pos, bsize, block, offset);

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
                cb(name, block, offset, sha.hex(), total, entry_mtime, entry_mode);
        }
        return r == ARCHIVE_EOF;
#endif
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
        const std::function<void(const std::string &, int64_t, int64_t, const std::string &,
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
            // Byte offset of this member's header within the tape file, split into
            // the physical block it lands in and the offset inside that block.
            const la_int64_t pos = archive_read_header_position(a);
            int64_t block = -1, offset = -1;
            split_block_pos(pos, bsize, block, offset);

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
            cb(name, block, offset, sha.hex(), total, entry_mtime, entry_mode);
        }
        return r == ARCHIVE_EOF;
    }

    bool tar_create_from_paths_with_blocks(
        const std::vector<std::string> &paths,
        struct archive *out, int64_t bsize,
        const std::function<void(const std::string &, int64_t, int64_t,
                                 const std::string &, uint64_t, time_t, mode_t)> &cb)
    {
        ArchiveReadPtr disk(archive_read_disk_new());
        if (!disk)
            return false;
        archive_read_disk_set_standard_lookup(disk.get());

        std::vector<std::byte> buf(kReadBlock);

        for (const auto &srcpath : paths)
        {
            ArchiveEntryPtr e(archive_entry_new());
            if (!e)
                return false;
            archive_entry_copy_sourcepath(e.get(), srcpath.c_str());
            // Set pathname before disk-entry fill so archive_entry_pathname()
            // is non-NULL; archive_read_disk_entry_from_file only fills stat
            // fields (size, mtime, mode etc.) and does not touch pathname.
            archive_entry_copy_pathname(e.get(), srcpath.c_str());
            if (archive_read_disk_entry_from_file(disk.get(), e.get(), -1, nullptr) != ARCHIVE_OK)
            {
                std::fprintf(stderr, "warning: %s: %s\n",
                             srcpath.c_str(), archive_error_string(disk.get()));
                continue;
            }

            const std::string name = normalize(archive_entry_pathname(e.get()));
            archive_entry_set_pathname_utf8(e.get(), name.c_str());

            const time_t mtime = archive_entry_mtime(e.get());
            const mode_t mode  = archive_entry_perm(e.get());

            if (archive_write_header(out, e.get()) != ARCHIVE_OK)
                return false;

#ifdef HAVE_ARCHIVE_WRITE_HEADER_POSITION
            const la_int64_t pos = archive_write_header_position(out);
            int64_t block = -1, offset = -1;
            split_block_pos(pos, bsize, block, offset);
#else
            (void)bsize;
            int64_t block = -1, offset = -1;
#endif

            if (archive_entry_filetype(e.get()) != AE_IFREG ||
                archive_entry_hardlink(e.get()) != nullptr)
                continue;

            Fd fd(::open(srcpath.c_str(), O_RDONLY));
            if (!fd.valid())
            {
                std::fprintf(stderr, "warning: cannot open %s: %s\n",
                             srcpath.c_str(), std::strerror(errno));
                continue;
            }

            security::Sha256 sha;
            uint64_t total = 0;
            ssize_t n;
            while ((n = ::read(fd.get(),
                               static_cast<void *>(buf.data()), buf.size())) > 0)
            {
                if (archive_write_data(out, buf.data(), static_cast<size_t>(n)) < 0)
                    return false;
                sha.update(buf.data(), static_cast<size_t>(n));
                total += static_cast<uint64_t>(n);
            }
            if (n < 0)
                return false;

            cb(name, block, offset, sha.hex(), total, mtime, mode);
        }
        return true;
    }

} // namespace tapir
