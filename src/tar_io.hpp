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
        time_t mtime = 0; // tar header mtime (0 → current time)
    };

    // Read a whole member into memory (e.g. manifest.json).
    bool tar_read_member(struct archive *a, const std::string &member, std::string &out);

    // Extract a member into a fresh anonymous temp file; yields its Fd + size.
    bool tar_extract_member(struct archive *a, const std::string &member, Fd &out_fd, uint64_t &out_size);

    // Write members into an opened write-archive (does NOT close it).
    bool tar_write_files(struct archive *a, const std::vector<OutFile> &files);
    bool tar_write_member(struct archive *a, const std::string &member, const std::string &data);

    // Stream every regular member. on_header(name) is called immediately after the
    // tar header is read (before any data is consumed); cb(name, sha256, size, mtime)
    // is called after the member's data has been fully read and hashed. on_header
    // may be empty — it is only invoked when non-null.
    bool tar_for_each_member(
        struct archive *a,
        const std::function<void(const std::string &name, const std::string &sha256, uint64_t size, time_t mtime)> &cb,
        const std::function<void(const std::string &name)> &on_header = {});

    // Copy every member from read-archive `in` to write-archive `out` (normalising
    // leading "./" or "/"), computing SHA-256 and invoking cb(name, sha256, size, mtime)
    // per regular file. For importing a tar from disk onto the tape.
    bool tar_copy_members(
        struct archive *in, struct archive *out,
        const std::function<void(const std::string &name, const std::string &sha256, uint64_t size, time_t mtime)> &cb);

} // namespace tapir

#endif // TAPIR_TAR_IO_HPP
