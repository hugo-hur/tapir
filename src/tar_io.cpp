#include "tar_io.hpp"
#include "security.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <cstdlib>

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
            archive_entry_set_perm(e.get(), kFileMode);
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
        if (archive_write_header(a, e.get()) != ARCHIVE_OK)
            return false;
        if (!data.empty() && archive_write_data(a, data.data(), data.size()) < 0)
            return false;
        return true;
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
        const std::function<void(const std::string &, const std::string &, uint64_t)> &cb)
    {
        struct archive_entry *e;
        int r;
        while ((r = archive_read_next_header(in, &e)) == ARCHIVE_OK)
        {
            const std::string name = normalize(epath(e));
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
                cb(name, sha.hex(), total);
        }
        return r == ARCHIVE_EOF;
    }

    bool tar_for_each_member(
        struct archive *a,
        const std::function<void(const std::string &, const std::string &, uint64_t)> &cb)
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
            cb(epath(e), sha.hex(), total);
        }
        return r == ARCHIVE_EOF;
    }

} // namespace tapir
