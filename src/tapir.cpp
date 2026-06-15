// tapir.cpp — read-only FUSE3 mount of a tar tape archive (initial version).
//
// This first cut targets the *file* archive that ltfs_to_tar.py produces for a
// non-tape destination: a data tar `<archive>.tar` plus a sibling index tar
// `<archive>.tar.manifest.tar` holding manifest.json. The manifest gives the
// directory tree + per-file size/sha256; file content is read back through
// libarchive on demand and cached to an anonymous temp file for random access.
//
// Metadata operations (getattr/readdir) are served entirely from the in-RAM
// manifest. Only content reads touch the archive, serialized behind a mutex
// (one tar reader today, one tape head later). The tape device path
// (mt seek to a per-file block) is the next step and is intentionally stubbed.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1           // expose strptime/timegm/pread/pwrite/mkstemp under -std=c++NN
#endif

#ifdef HAVE_CONFIG_H
#  include "config.h"           // defines FUSE_USE_VERSION before <fuse.h>
#endif

#include <fuse.h>

#include <archive.h>
#include <archive_entry.h>

#include "manifest.hpp"

#include <cerrno>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

using namespace tapir;

namespace {

struct CachedFile {
    int      fd   = -1;          // anonymous (unlinked) temp file holding the member bytes
    uint64_t size = 0;
};

struct State {
    std::string              data_path;     // path to the data .tar
    Manifest                 manifest;
    std::unique_ptr<Node>    root;
    time_t                   mtime = 0;
    uid_t                    uid = 0;
    gid_t                    gid = 0;

    std::mutex                       mtx;    // serialize archive scans + cache mutation
    std::map<std::string, CachedFile> cache; // member path -> extracted temp file
};

State* g = nullptr;

// ── libarchive helpers ────────────────────────────────────────────────────────

static struct archive* open_tar(const std::string& path) {
    struct archive* a = archive_read_new();
    archive_read_support_format_tar(a);     // ustar / pax / gnu — what the writer emits
    if (archive_read_open_filename(a, path.c_str(), 1 << 16) != ARCHIVE_OK) {
        archive_read_free(a);
        return nullptr;
    }
    return a;
}

// Return the member name as UTF-8. archive_entry_pathname() converts to the
// current locale's charset (mangling non-ASCII under the C locale); the _utf8
// variant returns the stored UTF-8 directly, which matches the manifest paths.
static const char* entry_path(struct archive_entry* e) {
    const char* p = archive_entry_pathname_utf8(e);
    return p ? p : archive_entry_pathname(e);
}

static bool entry_matches(const char* p, const std::string& name) {
    if (!p) return false;
    if (name == p) return true;
    // tolerate a leading "./" some tar writers prepend
    return p[0] == '.' && p[1] == '/' && name == (p + 2);
}

// Read a whole member into memory (used for the small manifest.json).
static bool read_member_to_memory(const std::string& tar, const std::string& name, std::string& out) {
    struct archive* a = open_tar(tar);
    if (!a) return false;
    struct archive_entry* e;
    bool found = false;
    while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
        if (entry_matches(entry_path(e), name)) {
            out.clear();
            const void* buff; size_t len; la_int64_t off; int r;
            while ((r = archive_read_data_block(a, &buff, &len, &off)) == ARCHIVE_OK)
                out.append(static_cast<const char*>(buff), len);
            found = (r == ARCHIVE_EOF);
            break;
        }
        archive_read_data_skip(a);
    }
    archive_read_free(a);
    return found;
}

// Extract a member to a fresh anonymous temp file; returns its fd + size.
static bool extract_member_to_tmpfd(const std::string& tar, const std::string& name,
                                    int& out_fd, uint64_t& out_size) {
    struct archive* a = open_tar(tar);
    if (!a) return false;
    struct archive_entry* e;
    bool ok = false;
    while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
        if (entry_matches(entry_path(e), name)) {
            char tmpl[] = "/tmp/tapir-cacheXXXXXX";
            int fd = mkstemp(tmpl);
            if (fd < 0) break;
            unlink(tmpl);                    // anonymous: space reclaimed on close
            const void* buff; size_t len; la_int64_t off; int r; bool werr = false;
            while ((r = archive_read_data_block(a, &buff, &len, &off)) == ARCHIVE_OK) {
                if (pwrite(fd, buff, len, static_cast<off_t>(off)) != static_cast<ssize_t>(len)) {
                    werr = true;
                    break;
                }
            }
            if (!werr && r == ARCHIVE_EOF) {
                la_int64_t sz = archive_entry_size(e);
                out_fd = fd;
                out_size = sz >= 0 ? static_cast<uint64_t>(sz) : 0;
                ok = true;
            } else {
                close(fd);
            }
            break;
        }
        archive_read_data_skip(a);
    }
    archive_read_free(a);
    return ok;
}

// ── FUSE operations ───────────────────────────────────────────────────────────

static void* t_init(struct fuse_conn_info*, struct fuse_config* cfg) {
    // The archive is immutable, so cache hard and never invalidate.
    cfg->kernel_cache     = 1;
    cfg->entry_timeout    = 86400;
    cfg->attr_timeout     = 86400;
    cfg->negative_timeout = 0;
    return nullptr;
}

static void t_destroy(void*) {
    if (!g) return;
    std::lock_guard<std::mutex> lk(g->mtx);
    for (auto& [name, c] : g->cache)
        if (c.fd >= 0) close(c.fd);
    g->cache.clear();
}

static void fill_stat(const Node* n, struct stat* st) {
    std::memset(st, 0, sizeof(*st));
    st->st_uid = g->uid;
    st->st_gid = g->gid;
    st->st_atime = st->st_mtime = st->st_ctime = g->mtime;
    if (n->is_dir) {
        st->st_mode  = S_IFDIR | 0555;
        st->st_nlink = 2;
    } else {
        st->st_mode   = S_IFREG | 0444;
        st->st_nlink  = 1;
        st->st_size   = static_cast<off_t>(n->size);
        st->st_blocks = static_cast<blkcnt_t>((n->size + 511) / 512);
    }
}

static int t_getattr(const char* path, struct stat* st, struct fuse_file_info*) {
    const Node* n = resolve(g->root.get(), path);
    if (!n) return -ENOENT;
    fill_stat(n, st);
    return 0;
}

static int t_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                     off_t, struct fuse_file_info*, enum fuse_readdir_flags) {
    const Node* n = resolve(g->root.get(), path);
    if (!n) return -ENOENT;
    if (!n->is_dir) return -ENOTDIR;

    filler(buf, ".",  nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
    filler(buf, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
    for (const auto& [name, child] : n->children) {
        struct stat st;
        fill_stat(child.get(), &st);   // full stat so readdirplus needs no extra getattr
        filler(buf, name.c_str(), &st, 0, FUSE_FILL_DIR_PLUS);
    }
    return 0;
}

static int t_open(const char* path, struct fuse_file_info* fi) {
    const Node* n = resolve(g->root.get(), path);
    if (!n) return -ENOENT;
    if (n->is_dir) return -EISDIR;
    if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EACCES;   // read-only filesystem
    return 0;
}

static int t_read(const char* path, char* buf, size_t size, off_t offset,
                  struct fuse_file_info*) {
    const Node* n = resolve(g->root.get(), path);
    if (!n) return -ENOENT;
    if (n->is_dir || !n->entry) return -EISDIR;

    int fd;
    uint64_t fsize;
    {
        std::lock_guard<std::mutex> lk(g->mtx);
        auto it = g->cache.find(n->entry->path);
        if (it == g->cache.end()) {
            int nfd; uint64_t nsz;
            if (!extract_member_to_tmpfd(g->data_path, n->entry->path, nfd, nsz)) {
                std::fprintf(stderr, "tapir: failed to extract '%s' from %s\n",
                             n->entry->path.c_str(), g->data_path.c_str());
                return -EIO;
            }
            it = g->cache.emplace(n->entry->path, CachedFile{nfd, nsz}).first;
        }
        fd    = it->second.fd;
        fsize = it->second.size;
    }

    if (static_cast<uint64_t>(offset) >= fsize) return 0;
    size_t want = size;
    if (static_cast<uint64_t>(offset) + want > fsize)
        want = static_cast<size_t>(fsize - static_cast<uint64_t>(offset));
    ssize_t r = pread(fd, buf, want, offset);
    if (r < 0) return -errno;
    return static_cast<int>(r);
}

const struct fuse_operations tapir_ops = {
    .getattr = t_getattr,
    .open    = t_open,
    .read    = t_read,
    .readdir = t_readdir,
    .init    = t_init,
    .destroy = t_destroy,
};

} // namespace

int main(int argc, char** argv) {
    std::setlocale(LC_ALL, "");   // honour the environment locale (typically UTF-8)

    if (argc < 3) {
        std::fprintf(stderr,
            "tapir — read-only FUSE mount of a tar archive (initial, file-mode)\n"
            "usage: %s <archive.tar> <mountpoint> [fuse options]\n"
            "  reads the index from <archive.tar>.manifest.tar\n", argv[0]);
        return 2;
    }

    std::string archive = argv[1];
    if (archive.rfind("/dev/", 0) == 0) {
        std::fprintf(stderr,
            "tapir: tape devices are not supported yet in this initial version.\n"
            "       Point at the data .tar file (with its sibling .manifest.tar).\n");
        return 2;
    }

    static State st;             // static: lives for the whole fuse_main run
    st.data_path = archive;
    st.uid   = geteuid();
    st.gid   = getegid();
    st.mtime = time(nullptr);

    const std::string manifest_tar = archive + ".manifest.tar";
    std::string manifest_json;
    if (!read_member_to_memory(manifest_tar, "manifest.json", manifest_json)) {
        std::fprintf(stderr, "tapir: could not read manifest.json from %s\n", manifest_tar.c_str());
        return 1;
    }
    try {
        st.manifest = Manifest::parse(manifest_json);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "tapir: %s\n", ex.what());
        return 1;
    }
    st.root = build_tree(st.manifest);

    if (!st.manifest.archives.empty()) {
        struct tm tmv{};
        if (strptime(st.manifest.archives.front().created.c_str(), "%Y-%m-%dT%H:%M:%S", &tmv))
            st.mtime = timegm(&tmv);
    }

    g = &st;
    std::fprintf(stderr, "tapir: %zu archive(s), %zu file(s) from %s\n",
                 st.manifest.archives.size(), st.manifest.files.size(), archive.c_str());

    // Pass through argv[0] + everything from argv[2] on (mountpoint + fuse opts).
    std::vector<char*> fargs;
    fargs.push_back(argv[0]);
    for (int i = 2; i < argc; ++i) fargs.push_back(argv[i]);
    return fuse_main(static_cast<int>(fargs.size()), fargs.data(),
                     &tapir_ops, nullptr);
}
