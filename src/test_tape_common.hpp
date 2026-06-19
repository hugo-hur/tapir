// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

// test_tape_common.hpp — shared helpers for the optional hardware tape tests
// (test_tape_seq, test_tape_seek). Test-only header: it pulls in `using
// namespace tapir` and defines small inline helpers, so it must only be
// included by a test's translation unit.
//
// Runtime parameters (no configure define needed):
//   TAPIR_TEST_DEVICE        no-rewind scratch tape device (…-nst). REQUIRED.
//   TAPIR_TEST_BLOCK_FACTOR  blocking factor ×512 (default 256 = 128 KiB).
//
//   make check                              -> SKIP (exit 77), no hardware touched
//   TAPIR_TEST_DEVICE=/dev/nst0 make check  -> runs against that drive
//
// The drive must be a SCRATCH tape in variable-block mode (`mt -f <dev> setblk 0`):
// the helper appends a data archive + manifest at end-of-tape.

#ifndef TAPIR_TEST_TAPE_COMMON_HPP
#define TAPIR_TEST_TAPE_COMMON_HPP

#include "tape.hpp"
#include "tar_io.hpp"
#include "index.hpp"
#include "raii.hpp"
#include "security.hpp"

#include <archive.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

using namespace tapir;

// Per-program tally (each test is its own executable).
inline int g_pass = 0, g_fail = 0;
inline void check(bool cond, const char *msg)
{
    if (cond) { ++g_pass; std::printf("  ok   %s\n", msg); }
    else { ++g_fail; std::fprintf(stderr, "  FAIL %s\n", msg); }
}
inline int finish() { std::printf("=== %d passed, %d failed ===\n", g_pass, g_fail); return g_fail ? 1 : 0; }

// One archive member: name, binary content, sha256, and (filled by a scan) its
// header position (block + within-block offset).
struct Member { std::string name; std::vector<std::byte> content; std::string sha;
                int64_t block = -1, offset = -1; bool scanned = false; };

// Deterministic, incompressible-ish binary content.
inline std::vector<std::byte> gen(std::size_t n, unsigned seed)
{
    std::vector<std::byte> v(n);
    unsigned x = seed * 2654435761u + 1;
    for (std::size_t i = 0; i < n; ++i) { x = x * 1103515245u + 12345u; v[i] = static_cast<std::byte>(x >> 16); }
    return v;
}

// Three members sized so not all start at block 0 (beta's data pushes gamma past
// the first physical block), which is what makes the per-member seek meaningful.
inline std::vector<Member> sample_members()
{
    return { {"alpha", gen(100, 1), ""}, {"beta", gen(200000, 2), ""}, {"gamma", gen(300, 3), ""} };
}

// Resolve the test device from the environment. Returns nullptr -> caller SKIPs.
inline const char *tape_test_device(int &bf)
{
    bf = 256;
    if (const char *b = std::getenv("TAPIR_TEST_BLOCK_FACTOR")) { int v = std::atoi(b); if (v > 0) bf = v; }
    const char *dev = std::getenv("TAPIR_TEST_DEVICE");
    return (dev && *dev) ? dev : nullptr;
}

// Anonymous temp file holding `content`, returned as an open Fd (read via pread).
inline Fd make_src(const std::vector<std::byte> &content)
{
    char tmpl[] = "/tmp/tapir-test-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) { ::unlink(tmpl); if (::write(fd, content.data(), content.size()) < 0) {} }
    return Fd(fd);
}

// Append the members as one multi-member data archive + manifest at EOD.
// Computes each member's sha; returns the data tape file via `dtf`.
inline bool write_archive(const char *dev, int bf, std::vector<Member> &ms, int &dtf)
{
    for (auto &m : ms) { security::Sha256 h; h.update(m.content.data(), m.content.size()); m.sha = h.hex(); }

    std::vector<Fd> srcs; srcs.reserve(ms.size());
    std::vector<OutFile> outs;
    for (auto &m : ms) { srcs.push_back(make_src(m.content)); outs.push_back({m.name, srcs.back().get(), m.content.size(), 0, 0}); }

    Tape tape(dev, bf);
    Index idx; idx.source = "test_tape"; idx.created = "2026-06-18T00:00:00";
    dtf = -1;
    return tape.append(
        bf,
        [&](struct archive *a) { return tar_write_files(a, outs); },
        [&](int data_dtf) {
            for (auto &m : ms) idx.add_file(m.name, m.content.size(), m.sha, data_dtf, bf, 0, 0);
            return idx.serialize(-1, bf);
        },
        dtf);
}

#endif // TAPIR_TEST_TAPE_COMMON_HPP
