// raii.hpp — compile-time constants, invariants, and RAII wrappers.
//
// Every OS/library resource the project owns (file descriptors, libarchive
// handles, OpenSSL digest contexts) is wrapped here so that ordinary code never
// calls close()/free()/delete itself. The *only* close()/*_free() calls in the
// whole codebase live in the deleters below.

#ifndef TAPIR_RAII_HPP
#define TAPIR_RAII_HPP

#include <archive.h>
#include <archive_entry.h>


#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <sys/types.h>
#include <unistd.h>

namespace tapir
{

    // ── compile-time constants + invariants ──────────────────────────────────────
    inline constexpr std::size_t kTarBlock = 512;       // tar record size
    inline constexpr std::size_t kReadBlock = 1u << 16; // libarchive read buffer
    inline constexpr ::mode_t kDirMode = 0555;          // dirs: read + traverse
    inline constexpr ::mode_t kFileMode = 0444;         // sealed files: read-only
    inline constexpr ::mode_t kNewFileMode = 0644;      // a file currently being written

    static_assert(sizeof(off_t) == 8,
                  "tapir needs a 64-bit off_t — build with _FILE_OFFSET_BITS=64 (FUSE_CFLAGS)");
    static_assert(kReadBlock % kTarBlock == 0, "read buffer must be a whole number of tar records");
    static_assert(kFileMode == 0444 && kDirMode == 0555, "read-only mode bits");

    // constexpr hex nibble — used to format digests; checked at compile time.
    constexpr char hex_digit(unsigned v) { return v < 10 ? char('0' + v) : char('a' + (v - 10)); }
    static_assert(hex_digit(0) == '0' && hex_digit(10) == 'a' && hex_digit(15) == 'f');

    // ── owning file descriptor ────────────────────────────────────────────────────
    class Fd
    {
    public:
        Fd() = default;
        explicit Fd(int fd) : fd_(fd) {}
        Fd(const Fd &) = delete;
        Fd &operator=(const Fd &) = delete;
        Fd(Fd &&o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
        Fd &operator=(Fd &&o) noexcept
        {
            if (this != &o)
            {
                reset();
                fd_ = o.fd_;
                o.fd_ = -1;
            }
            return *this;
        }
        ~Fd() { reset(); }
        int get() const { return fd_; }
        bool valid() const { return fd_ >= 0; }
        void reset()
        {
            if (fd_ >= 0)
            {
                ::close(fd_);
                fd_ = -1;
            }
        } // the only close() in the codebase
    private:
        int fd_ = -1;
    };

    // ── libarchive / OpenSSL handles ──────────────────────────────────────────────
    struct ArchiveReadDeleter
    {
        void operator()(struct archive *a) const noexcept
        {
            if (a)
                archive_read_free(a);
        }
    };
    struct ArchiveWriteDeleter
    {
        void operator()(struct archive *a) const noexcept
        {
            if (a)
                archive_write_free(a);
        }
    };
    struct ArchiveEntryDeleter
    {
        void operator()(struct archive_entry *e) const noexcept
        {
            if (e)
                archive_entry_free(e);
        }
    };
    struct EvpCtxDeleter
    {
        void operator()(EVP_MD_CTX *c) const noexcept
        {
            if (c)
                EVP_MD_CTX_free(c);
        }
    };

    using ArchiveReadPtr = std::unique_ptr<struct archive, ArchiveReadDeleter>;
    using ArchiveWritePtr = std::unique_ptr<struct archive, ArchiveWriteDeleter>;
    using ArchiveEntryPtr = std::unique_ptr<struct archive_entry, ArchiveEntryDeleter>;
    
} // namespace tapir

#endif // TAPIR_RAII_HPP
