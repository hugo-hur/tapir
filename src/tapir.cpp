// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

// tapir.cpp — FUSE3 mount of a tar tape archive.
//
// Serves a read/write filesystem backed by a tape drive. Metadata lives in an
// in-RAM Index loaded from the latest on-tape manifest at mount time. Reads are
// cached locally after the first tape stream. Writes go through a background
// WriterThread (see writer.hpp): on file close the data is written to tape
// asynchronously; a manifest is written at the first fsync or unmount. Deletes
// are index-only; existing tape data is never rewritten.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <fuse.h>

#include "index.hpp"
#include "tape.hpp"
#include "tar_io.hpp"
#include "security.hpp"
#include "writer.hpp"

#include <cerrno>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static_assert(FUSE_USE_VERSION >= 30, "tapir requires the libfuse 3 API");

using namespace tapir;

namespace tapir
{
    struct WriteHandle
    {
        std::string path;
        Node *node = nullptr; // non-owning: lives in the Index tree
        Fd fd;                // staging temp file (anonymous)
        uint64_t pos = 0;     // next expected (sequential) write offset
        security::Sha256 sha;
    };
} // namespace tapir

namespace
{

    struct CacheEntry
    {
        Fd fd;
        uint64_t size = 0;
    };

    struct State
    {
        std::unique_ptr<Tape> tape;
        int block_factor = 512;
        Index index;
        time_t mtime = 0;
        uid_t uid = 0;
        gid_t gid = 0;

        std::mutex mtx;
        std::map<std::string, CacheEntry> cache;
        std::map<uint64_t, std::unique_ptr<WriteHandle>> writers;
        uint64_t next_h = 1;

        std::unique_ptr<WriterThread> writer;
    };

    State *g = nullptr;

    // ── helpers ───────────────────────────────────────────────────────────────────

    constexpr const char *member_name(const char *path)
    {
        return (path && path[0] == '/') ? path + 1 : path;
    }

    Fd make_temp()
    {
        char tmpl[] = "/tmp/tapir-writeXXXXXX";
        int fd = mkstemp(tmpl);
        if (fd >= 0)
            ::unlink(tmpl);
        return Fd(fd);
    }

    void fill_stat(const Node *n, struct stat *st)
    {
        std::memset(st, 0, sizeof(*st));
        st->st_uid = g->uid;
        st->st_gid = g->gid;
        st->st_atime = st->st_ctime = g->mtime;
        if (n->is_dir)
        {
            st->st_mtime = g->mtime;
            st->st_mode = S_IFDIR | kDirMode;
            st->st_nlink = 2;
        }
        else
        {
            st->st_mtime = n->mtime ? n->mtime : g->mtime;
            const mode_t sealed = n->mode ? (n->mode & ~0222u) : kFileMode;
            st->st_mode = S_IFREG | (n->writing ? kNewFileMode : sealed);
            st->st_nlink = 1;
            st->st_size = static_cast<off_t>(n->size);
            st->st_blocks = static_cast<blkcnt_t>((n->size + 511) / 512);
        }
    }

    // Flush the index to tape. Temporarily releases `lk` while the writer thread
    // works; reacquires before return. No-op if the index is not dirty.
    static void sync_locked(std::unique_lock<std::mutex> &lk)
    {
        g->writer->sync(lk);
    }

    // ── FUSE ops ──────────────────────────────────────────────────────────────────

    void *t_init(struct fuse_conn_info *, struct fuse_config *cfg)
    {
        cfg->kernel_cache = 1;
        cfg->entry_timeout = 1;
        cfg->attr_timeout = 1;
        cfg->negative_timeout = 0;
        return nullptr;
    }

    void t_destroy(void *)
    {
        if (!g)
            return;
        {
            std::unique_lock<std::mutex> lk(g->mtx);
            sync_locked(lk);
            g->cache.clear();
            g->writers.clear();
        } // lk released here — writer thread must not hold g->mtx when joined
        g->writer.reset(); // request_stop() + join via ~jthread
    }

    int t_getattr(const char *path, struct stat *st, struct fuse_file_info *)
    {
        std::lock_guard<std::mutex> lk(g->mtx);
        const Node *n = g->index.resolve(path);
        if (!n)
            return -ENOENT;
        fill_stat(n, st);
        return 0;
    }

    int t_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                  off_t, struct fuse_file_info *, enum fuse_readdir_flags)
    {
        std::lock_guard<std::mutex> lk(g->mtx);
        const Node *n = g->index.resolve(path);
        if (!n)
            return -ENOENT;
        if (!n->is_dir)
            return -ENOTDIR;
        filler(buf, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
        filler(buf, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
        for (const auto &[name, child] : n->children)
        {
            struct stat st;
            fill_stat(child.get(), &st);
            filler(buf, name.c_str(), &st, 0, FUSE_FILL_DIR_PLUS);
        }
        return 0;
    }

    int t_open(const char *path, struct fuse_file_info *fi)
    {
        std::lock_guard<std::mutex> lk(g->mtx);
        const Node *n = g->index.resolve(path);
        if (!n)
            return -ENOENT;
        if (n->is_dir)
            return -EISDIR;
        if ((fi->flags & O_ACCMODE) != O_RDONLY)
            return -EPERM; // sealed: read-or-delete only
        fi->fh = 0;
        return 0;
    }

    int t_create(const char *path, mode_t mode, struct fuse_file_info *fi)
    {
        std::lock_guard<std::mutex> lk(g->mtx);
        if (g->index.resolve(path))
            return -EEXIST;
        Node *n = g->index.create_file(path);
        if (!n)
            return -ENOENT;
        n->mtime = std::time(nullptr);
        n->mode = mode;
        auto h = std::make_unique<WriteHandle>();
        h->path = path;
        h->node = n;
        h->fd = make_temp();
        if (!h->fd.valid())
            return -EIO;
        n->writing = h.get();
        const uint64_t id = g->next_h++;
        g->writers.emplace(id, std::move(h));
        fi->fh = id;
        return 0;
    }

    int t_write(const char *, const char *buf, size_t size, off_t off,
                struct fuse_file_info *fi)
    {
        std::lock_guard<std::mutex> lk(g->mtx);
        auto it = g->writers.find(fi->fh);
        if (it == g->writers.end())
            return -EBADF;
        WriteHandle *h = it->second.get();
        if (static_cast<uint64_t>(off) != h->pos)
            return -EINVAL; // sequential only
        const ssize_t w = pwrite(h->fd.get(), buf, size, off);
        if (w < 0)
            return -errno;
        h->sha.update(buf, static_cast<size_t>(w));
        h->pos += static_cast<uint64_t>(w);
        if (h->node)
            h->node->size = h->pos;
        return static_cast<int>(w);
    }

    int t_truncate(const char *path, off_t size, struct fuse_file_info *)
    {
        std::lock_guard<std::mutex> lk(g->mtx);
        Node *n = g->index.resolve(path);
        if (!n)
            return -ENOENT;
        if (n->is_dir)
            return -EISDIR;
        if (n->writing && size == 0 && n->writing->pos == 0)
            return 0;
        if (n->writing && static_cast<uint64_t>(size) == n->writing->pos)
            return 0;
        return -EPERM;
    }

    int t_read(const char *path, char *buf, size_t size, off_t offset,
               struct fuse_file_info *)
    {
        std::lock_guard<std::mutex> lk(g->mtx);
        Node *n = g->index.resolve(path);
        if (!n)
            return -ENOENT;
        if (n->is_dir)
            return -EISDIR;

        int fd;
        uint64_t fsize;
        if (n->writing)
        {
            fd = n->writing->fd.get();
            fsize = n->size;
        }
        else if (n->staged)
        {
            // File closed but writer thread still in flight: serve from temp fd.
            // tar_write_files uses pread, so concurrent access is safe.
            fd = n->staged->fd.get();
            fsize = n->staged->size;
        }
        else
        {
            auto it = g->cache.find(path);
            if (it == g->cache.end())
            {
                Fd nf;
                uint64_t nsz;
                if (!g->tape->read_member(n->data_tape_file, n->block_factor, n->block_number,
                                          n->block_offset, member_name(path), nf, nsz))
                    return -EIO;
                it = g->cache.emplace(path, CacheEntry{std::move(nf), nsz}).first;
            }
            fd = it->second.fd.get();
            fsize = it->second.size;
        }

        if (static_cast<uint64_t>(offset) >= fsize)
            return 0;
        size_t want = size;
        if (static_cast<uint64_t>(offset) + want > fsize)
            want = static_cast<size_t>(fsize - static_cast<uint64_t>(offset));
        const ssize_t r = pread(fd, buf, want, offset);
        return r < 0 ? -errno : static_cast<int>(r);
    }

    // On file close, hand the temp fd to the WriterThread for async tape write.
    // n->staged keeps the fd alive for t_read while the write is in progress.
    // TODO: make 0-size files index-only to allow later data appends.
    int t_release(const char *, struct fuse_file_info *fi)
    {
        std::lock_guard<std::mutex> lk(g->mtx);
        auto it = g->writers.find(fi->fh);
        if (it == g->writers.end())
            return 0;
        WriteHandle *h = it->second.get();
        if (h->node)
        {
            h->node->sha256 = h->sha.hex();
            h->node->size   = h->pos;
            h->node->mtime  = std::time(nullptr);
            h->node->writing = nullptr;

            auto st = std::make_shared<Staged>();
            st->size  = h->pos;
            st->mtime = h->node->mtime;
            st->fd    = std::move(h->fd);
            h->node->staged = st;

            const time_t snap_mtime = h->node->mtime; // snapshot before any utimens race
            const std::string member{member_name(h->path.c_str())};
            g->writer->enqueue_file(h->node, std::move(st), member, snap_mtime);
            g->index.mark_dirty();
        }
        g->writers.erase(it);
        return 0;
    }

    int t_unlink(const char *path)
    {
        std::lock_guard<std::mutex> lk(g->mtx);
        g->cache.erase(path);
        return g->index.unlink_file(path) ? 0 : -ENOENT;
    }

    int t_mkdir(const char *path, mode_t)
    {
        std::lock_guard<std::mutex> lk(g->mtx);
        if (g->index.resolve(path))
            return -EEXIST;
        return g->index.make_dir(path) ? 0 : -ENOENT;
    }

    int t_rmdir(const char *path)
    {
        std::lock_guard<std::mutex> lk(g->mtx);
        const Node *n = g->index.resolve(path);
        if (!n)
            return -ENOENT;
        if (!n->is_dir)
            return -ENOTDIR;
        if (!n->children.empty())
            return -ENOTEMPTY;
        return g->index.remove_dir(path) ? 0 : -ENOTEMPTY;
    }

    // chmod on staged/writing files updates the stored mode (carried into the tar header).
    // Sealed on-tape files are immutable — reject with EPERM.
    static int t_chmod(const char *path, mode_t mode, struct fuse_file_info *)
    {
        std::lock_guard<std::mutex> lk(g->mtx);
        Node *n = g->index.resolve(path);
        if (!n)
            return -ENOENT;
        if (n->is_dir)
            return -EPERM;
        if (!n->staged && !n->writing)
            return -EPERM;
        n->mode = mode;
        return 0;
    }

    // mv(1) and touch(1) call utimensat(). Staged/writing nodes apply the new
    // mtime to Node + Staged; sealed on-tape files are immutable.
    static int t_utimens(const char *path, const struct timespec tv[2],
                         struct fuse_file_info *)
    {
        std::lock_guard<std::mutex> lk(g->mtx);
        Node *n = g->index.resolve(path);
        if (!n)
            return -ENOENT;
        if (!n->is_dir && !n->staged && !n->writing)
            return -EPERM;
        if (tv)
        {
            const time_t t = (tv[1].tv_nsec == UTIME_NOW)  ? std::time(nullptr)
                           : (tv[1].tv_nsec == UTIME_OMIT) ? n->mtime
                           : tv[1].tv_sec;
            n->mtime = t;
            if (n->staged)
                n->staged->mtime = t;
        }
        return 0;
    }

    static int t_fsync(const char *, int, struct fuse_file_info *)
    {
        std::unique_lock<std::mutex> lk(g->mtx);
        sync_locked(lk);
        return 0;
    }

    const struct fuse_operations tapir_ops = {
        .getattr  = t_getattr,
        .mkdir    = t_mkdir,
        .unlink   = t_unlink,
        .rmdir    = t_rmdir,
        .chmod    = t_chmod,
        .truncate = t_truncate,
        .open     = t_open,
        .read     = t_read,
        .write    = t_write,
        .release  = t_release,
        .fsync    = t_fsync,
        .readdir  = t_readdir,
        .fsyncdir = t_fsync,
        .init     = t_init,
        .destroy  = t_destroy,
        .create   = t_create,
        .utimens  = t_utimens,
    };

} // namespace

int main(int argc, char **argv)
{
    std::setlocale(LC_ALL, "");

    std::string device;
    int bf = 512;
    std::vector<char *> fargs;
    fargs.push_back(argv[0]);
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if ((a == "-b" || a == "--block-factor") && i + 1 < argc)
        {
            bf = std::atoi(argv[++i]);
            continue;
        }
        if (device.empty() && !a.empty() && a[0] != '-')
        {
            device = a;
            continue;
        }
        fargs.push_back(argv[i]);
    }
    if (device.empty() || fargs.size() < 2)
    {
        std::fprintf(stderr,
                     "tapir -- read/append/delete FUSE mount of a tar tape archive\n"
                     "usage: %s <tape-device> <mountpoint> [-b N] [fuse options]\n",
                     argv[0]);
        return 2;
    }
    if (device.rfind("/dev/", 0) != 0)
    {
        std::fprintf(stderr, "tapir: expected a tape device (...-nst), got %s\n", device.c_str());
        return 2;
    }

    static State st;
    st.block_factor = bf;
    st.tape = std::make_unique<Tape>(device, bf);
    st.uid  = geteuid();
    st.gid  = getegid();
    st.mtime = time(nullptr);

    std::string manifest_json;
    std::fprintf(stderr, "tapir: searching for manifest at end of tape %s...\n", device.c_str());
    if (!st.tape->read_latest_manifest(manifest_json))
    {
        // Probe with legacy (filename-only) reader to distinguish "no manifest at
        // all" from "old-format manifest without PAX magic".
        std::string probe;
        if (st.tape->read_latest_manifest_legacy(probe))
            std::fprintf(stderr,
                         "tapir: ERROR: tape %s has an old-format manifest (manifest.json without\n"
                         "       the tapir PAX magic header). Run 'tfsck --upgrade-manifest %s'\n"
                         "       to rewrite it with the required magic, then retry.\n",
                         device.c_str(), device.c_str());
        else
            std::fprintf(stderr, "tapir: could not read a manifest from %s\n", device.c_str());
        return 1;
    }
    try
    {
        st.index.load(manifest_json);
    }
    catch (const std::exception &ex)
    {
        std::fprintf(stderr, "tapir: %s\n", ex.what());
        return 1;
    }

    g = &st; // must precede WriterThread construction (thread reads g on first wake)
    st.writer = std::make_unique<WriterThread>(*st.tape, bf, st.mtx, st.index);

    const auto flat = st.index.flat();
    int slow_reads = 0; // files lacking a full (block + within-block offset) location
    for (const auto &f : flat)
        if (f.block_number < 0 || f.block_offset < 0)
            ++slow_reads;

    std::fprintf(stderr, "tapir: mounted %s -- volume %s, generation %llu, %zu file(s)\n",
                 device.c_str(),
                 st.index.volume_uuid().c_str(),
                 static_cast<unsigned long long>(st.index.latest_generation()),
                 flat.size());
    if (slow_reads > 0)
        std::fprintf(stderr,
                     "tapir: NOTE: %d file(s) lack a within-block offset — reads of them use a\n"
                     "       slower full-file scan (results are correct, just not seeked). Run:\n"
                     "           tfsck %s -m %d\n"
                     "       to record exact offsets and enable fast per-member reads.\n",
                     slow_reads, device.c_str(), bf);
    return fuse_main(static_cast<int>(fargs.size()), fargs.data(), &tapir_ops, nullptr);
}
