// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

// tar_io.hpp — libarchive helpers over an *already-opened* archive handle.
//
// These never open/close the archive themselves — the caller (tape.cpp) owns the
// device fd and the libarchive handle, so the same helpers work for a tape file
// or any other stream. Part of libtapir.

#ifndef TAPIR_TAR_IO_HPP
#define TAPIR_TAR_IO_HPP

#include "raii.hpp"

#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

struct archive; // libarchive (opaque)

namespace tapir
{

    // A file to be written into a tar; bytes are pread() from `fd` starting at 0.
    struct OutFile
    {
        std::string name; // member name (full relpath, no leading '/')
        int fd;           // borrowed readable fd
        uint64_t size;
        time_t mtime = 0;   // tar header mtime (0 → current time)
        mode_t mode = 0;    // permission bits (0 → use kFileMode default)
    };

    // Read a whole member into memory (e.g. manifest.json).
    bool tar_read_member(struct archive *a, const std::string &member, std::string &out);

    // Extract a member into a fresh anonymous temp file; yields its Fd + size.
    bool tar_extract_member(struct archive *a, const std::string &member, Fd &out_fd, uint64_t &out_size);

    // Open a read-archive on `fd` whose current position is the START of the
    // physical block holding a member's tar header, where the header begins
    // `block_offset` bytes into that block. The first block (`bsize` bytes) is read
    // whole and libarchive is fed from the header onward, then it continues reading
    // whole blocks from `fd`. Needed because a tape read returns one entire physical
    // block — libarchive cannot be started mid-block by a short read. Use after
    // positioning to a member's block (MTFSR on tape, lseek on a file). `fd` is
    // borrowed (not closed). Returns nullptr on error. Pairs with the per-member
    // tape_block + within-block offset recorded in the index.
    ArchiveReadPtr tar_open_at_block_offset(int fd, int bsize, int64_t block_offset);

    // Write members into an opened write-archive (does NOT close it).
    bool tar_write_files(struct archive *a, const std::vector<OutFile> &files);
    bool tar_write_member(struct archive *a, const std::string &member, const std::string &data);

    // PAX extended-header magic written into every tapir manifest tape file and
    // used (together with the member name) to distinguish tapir index files from
    // regular tar archives that happen to contain a "manifest.json" member.
    inline constexpr const char *kTapirMagicXattrName  = "user.tapir.magic";
    inline constexpr const char *kTapirMagicXattrValue = "tapir-index-v1";

    // Returns true if the archive entry carries the tapir manifest PAX magic xattr.
    bool tar_entry_has_tapir_magic(struct archive_entry *e);

    // Stream every regular member. on_header(name, is_tapir_index) is called
    // immediately after the tar header is read (before any data is consumed);
    // is_tapir_index is true when the member is "manifest.json" AND carries the
    // tapir PAX magic xattr — i.e. this tape file is a tapir manifest, not data.
    // cb(name, sha256, size, mtime, mode) is called after the member's data has
    // been fully read and hashed. on_header may be empty.
    bool tar_for_each_member(
        struct archive *a,
        const std::function<void(const std::string &name, const std::string &sha256,
                                 uint64_t size, time_t mtime, mode_t mode)> &cb,
        const std::function<void(const std::string &name, bool is_tapir_index)> &on_header = {});

    // Like tar_for_each_member but also reports where each member's tar header sits:
    // `block` is the 0-based physical block (header byte position / bsize) and
    // `offset` is the header's byte offset within that block (position % bsize),
    // both taken from archive_read_header_position(). The (block, offset) pair is
    // what tar_open_at_block_offset needs to seek straight to a member.
    bool tar_for_each_member_with_blocks(
        struct archive *a, int64_t bsize,
        const std::function<void(const std::string &name, int64_t block, int64_t offset,
                                 const std::string &sha256, uint64_t size,
                                 time_t mtime, mode_t mode)> &cb,
        const std::function<void(const std::string &name, int64_t block,
                                 bool is_tapir_index)> &on_header = {});

    // Copy every member from read-archive `in` to write-archive `out` (normalising
    // leading "./" or "/"), computing SHA-256 and invoking cb(name, sha256, size, mtime, mode)
    // per regular file. For importing a tar from disk onto the tape.
    bool tar_copy_members(
        struct archive *in, struct archive *out,
        const std::function<void(const std::string &name, const std::string &sha256,
                                 uint64_t size, time_t mtime, mode_t mode)> &cb);

    // Like tar_copy_members but also reports each member's write-side header position
    // via archive_write_header_position() (tapir libarchive fork).
    // `block` = pos / bsize, `offset` = pos % bsize — the same pair that
    // tar_open_at_block_offset needs to seek straight to the member on tape.
    // Requires HAVE_ARCHIVE_WRITE_HEADER_POSITION (--with-libarchive=bundled).
    bool tar_copy_members_with_blocks(
        struct archive *in, struct archive *out, int64_t bsize,
        const std::function<void(const std::string &name, int64_t block, int64_t offset,
                                 const std::string &sha256,
                                 uint64_t size, time_t mtime, mode_t mode)> &cb);

    // Create a tar in write-archive `out` from a list of disk paths, one entry per
    // path.  Each path is stat'd via archive_read_disk_entry_from_file() and its
    // data read with read().  Non-regular-file entries (dirs, symlinks) are written
    // as header-only.  Files that cannot be opened are skipped with a warning.
    // Calls cb(name, block, offset, sha256, size, mtime, mode) per regular file,
    // where block/offset come from archive_write_header_position() (falls back to
    // -1/-1 when the fork feature is absent).  Returns false only on a fatal archive
    // write error; per-file access errors are non-fatal.
    bool tar_create_from_paths_with_blocks(
        const std::vector<std::string> &paths,
        struct archive *out, int64_t bsize,
        const std::function<void(const std::string &name, int64_t block, int64_t offset,
                                 const std::string &sha256,
                                 uint64_t size, time_t mtime, mode_t mode)> &cb);

} // namespace tapir

#endif // TAPIR_TAR_IO_HPP
