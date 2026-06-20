// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

// test_tar_io.cpp — DEVICE-FREE unit tests for tar_io helpers that previously had
// only hardware coverage (phase 4 of the tape suite). Builds small tars in temp
// files and exercises:
//   - the tapir PAX magic round-trip (tar_write_member adds it, tar_entry_has_
//     tapir_magic detects it) — the basis of index detection;
//   - is_tapir_index = (member is "manifest.json" AND carries the magic), incl.
//     the negative: a magic-less manifest.json is NOT a tapir index;
//   - tar_read_member content round-trip;
//   - member-name normalisation (leading "./" and "/" stripped on read).
//
// Always runs (no TAPIR_TEST_DEVICE needed). Run via: make check

#include "test_tape_common.hpp"   // check(), gen(), make_src(), Fd, finish()

#include <archive_entry.h>

#include <cstring>
#include <functional>

// Fresh temp path (the file is created empty; the archive writer overwrites it).
static std::string mktemp_path()
{
    char tmpl[] = "/tmp/tapir-tario-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) ::close(fd);
    return std::string(tmpl);
}

// Build a pax tar at `path` via a writer callback; returns true on success.
static bool build(const std::string &path, const std::function<bool(struct archive *)> &w)
{
    struct archive *a = archive_write_new();
    archive_write_set_format_pax_restricted(a);
    bool ok = (archive_write_open_filename(a, path.c_str()) == ARCHIVE_OK) && w(a);
    archive_write_close(a);
    archive_write_free(a);
    return ok;
}

static struct archive *open_read_file(const std::string &path)
{
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, path.c_str(), 65536) != ARCHIVE_OK) { archive_read_free(a); return nullptr; }
    return a;
}

int main()
{
    std::printf("=== test_tar_io (no device) ===\n");
    const std::vector<std::byte> body = gen(1234, 7);

    // Archive A: manifest.json written via tar_write_member (gets the magic xattr)
    // + a plain data member written via tar_write_file (no magic).
    const std::string pathA = mktemp_path();
    {
        Fd src = make_src(body);
        bool ok = build(pathA, [&](struct archive *a) {
            if (!tar_write_member(a, "manifest.json", "{\"tapir\":1}")) return false;
            OutFile of{"data.bin", src.get(), body.size(), 0, 0};
            int64_t blk, off;
            return tar_write_file(a, of, 512, blk, off);
        });
        check(ok, "build archive A (magic manifest + plain data)");
    }

    // (1) tar_entry_has_tapir_magic on each raw header.
    {
        struct archive *a = open_read_file(pathA);
        check(a != nullptr, "open archive A");
        struct archive_entry *e;
        int seen = 0;
        bool man_magic = false, data_magic = true;
        while (a && archive_read_next_header(a, &e) == ARCHIVE_OK) {
            const char *p = archive_entry_pathname(e);
            const std::string n = p ? p : "";
            if (n == "manifest.json") { man_magic = tar_entry_has_tapir_magic(e); ++seen; }
            else if (n == "data.bin") { data_magic = tar_entry_has_tapir_magic(e); ++seen; }
            archive_read_data_skip(a);
        }
        if (a) archive_read_free(a);
        check(seen == 2, "saw both members of A");
        check(man_magic, "manifest.json carries the tapir magic xattr");
        check(!data_magic, "data.bin has no tapir magic");
    }

    // (2) is_tapir_index via tar_for_each_member_with_blocks (name AND magic).
    {
        struct archive *a = open_read_file(pathA);
        bool man_idx = false, data_idx = true;
        tar_for_each_member_with_blocks(
            a, 512,
            [&](const std::string &, int64_t, int64_t, const std::string &, uint64_t, time_t, mode_t) {},
            [&](const std::string &name, int64_t, bool is_idx) {
                if (name == "manifest.json") man_idx = is_idx;
                else if (name == "data.bin") data_idx = is_idx;
            });
        if (a) archive_read_free(a);
        check(man_idx, "for_each: manifest.json flagged is_tapir_index");
        check(!data_idx, "for_each: data.bin not flagged is_tapir_index");
    }

    // (3) a manifest.json WITHOUT magic must NOT be treated as a tapir index.
    {
        const std::string pathB = mktemp_path();
        Fd src = make_src(body);
        bool ok = build(pathB, [&](struct archive *a) {
            OutFile of{"manifest.json", src.get(), body.size(), 0, 0};
            int64_t blk, off;
            return tar_write_file(a, of, 512, blk, off); // plain write, no magic
        });
        check(ok, "build archive B (magic-less manifest.json)");
        struct archive *a = open_read_file(pathB);
        bool idx = true;
        tar_for_each_member_with_blocks(
            a, 512,
            [&](const std::string &, int64_t, int64_t, const std::string &, uint64_t, time_t, mode_t) {},
            [&](const std::string &name, int64_t, bool is_idx) { if (name == "manifest.json") idx = is_idx; });
        if (a) archive_read_free(a);
        check(!idx, "magic-less manifest.json is NOT a tapir index");
        ::unlink(pathB.c_str());
    }

    // (4) tar_read_member round-trips the member's bytes.
    {
        const std::string pathC = mktemp_path();
        const std::string payload = "hello tapir manifest payload \x01\x02\x03 end";
        check(build(pathC, [&](struct archive *a) { return tar_write_member(a, "manifest.json", payload); }),
              "build archive C");
        struct archive *a = open_read_file(pathC);
        std::string out;
        bool got = a && tar_read_member(a, "manifest.json", out);
        if (a) archive_read_free(a);
        check(got && out == payload, "tar_read_member round-trips content");
        ::unlink(pathC.c_str());
    }

    // (5) name normalisation: reported names have no leading "./" or "/".
    {
        const std::string pathD = mktemp_path();
        Fd s1 = make_src(gen(10, 1)), s2 = make_src(gen(10, 2)),
           s3 = make_src(gen(10, 3)), s4 = make_src(gen(10, 4));
        bool ok = build(pathD, [&](struct archive *a) {
            int64_t b, o;
            return tar_write_file(a, OutFile{"./foo",   s1.get(), 10, 0, 0}, 512, b, o)
                && tar_write_file(a, OutFile{"/qux",    s2.get(), 10, 0, 0}, 512, b, o)
                && tar_write_file(a, OutFile{"sub/bar", s3.get(), 10, 0, 0}, 512, b, o)
                && tar_write_file(a, OutFile{"baz",     s4.get(), 10, 0, 0}, 512, b, o);
        });
        check(ok, "build archive D (prefixed names)");
        struct archive *a = open_read_file(pathD);
        std::vector<std::string> names;
        tar_for_each_member_with_blocks(
            a, 512,
            [&](const std::string &n, int64_t, int64_t, const std::string &, uint64_t, time_t, mode_t) {
                names.push_back(n);
            },
            {});
        if (a) archive_read_free(a);
        bool has_foo = false, has_qux = false, has_bar = false, has_baz = false, dirty = false;
        for (const auto &n : names) {
            if (n == "foo") has_foo = true;
            if (n == "qux") has_qux = true;
            if (n == "sub/bar") has_bar = true;
            if (n == "baz") has_baz = true;
            if (n.rfind("./", 0) == 0 || (!n.empty() && n[0] == '/')) dirty = true;
        }
        check(has_foo, "normalize: './foo' -> 'foo'");
        check(has_qux, "normalize: '/qux' -> 'qux'");
        check(has_bar, "normalize: internal slash 'sub/bar' kept");
        check(has_baz, "normalize: 'baz' unchanged");
        check(!dirty, "no reported name retains a leading './' or '/'");
        ::unlink(pathD.c_str());
    }

    ::unlink(pathA.c_str());
    return finish();
}
