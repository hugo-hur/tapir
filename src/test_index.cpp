// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

// test_index.cpp — unit tests for Index::add_file last-occurrence-wins semantics.
// Run via: make check

#include "index.hpp"

#include <cstdio>
#include <memory>
#include <string>

using namespace tapir;

static int g_pass = 0, g_fail = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (expr) { ++g_pass; }                                                  \
        else { ++g_fail;                                                         \
               std::fprintf(stderr, "FAIL: %s  (%s:%d)\n",                      \
                            #expr, __FILE__, __LINE__); }                        \
    } while (0)

// ── helpers ───────────────────────────────────────────────────────────────────

static Index make_index()
{
    Index idx;
    idx.source = "test";
    idx.created = "2026-01-01T00:00:00";
    return idx;
}

// ── tests ─────────────────────────────────────────────────────────────────────

static void test_add_new_file()
{
    std::puts("-- add_file: new path is indexed");
    Index idx = make_index();
    idx.add_file("data/hello.txt", 5, "sha_a", 0, 20, 1000, 0644);
    const Node *n = idx.resolve("data/hello.txt");
    CHECK(n != nullptr);
    CHECK(!n->is_dir);
    CHECK(n->size == 5);
    CHECK(n->sha256 == "sha_a");
    CHECK(n->mtime == 1000);
    CHECK(n->data_tape_file == 0);
}

static void test_last_occurrence_wins()
{
    std::puts("-- add_file: second call with same path overwrites first (last wins)");
    Index idx = make_index();

    // First occurrence — older version on tape
    idx.add_file("docs/readme.txt", 10, "sha_old", 0, 20, 1000, 0644);
    // Second occurrence — newer version appended to tape
    idx.add_file("docs/readme.txt", 42, "sha_new", 0, 20, 9999, 0755);

    const Node *n = idx.resolve("docs/readme.txt");
    CHECK(n != nullptr);
    CHECK(n->size == 42);
    CHECK(n->sha256 == "sha_new");
    CHECK(n->mtime == 9999);
    CHECK(n->mode == 0755);
}

static void test_last_occurrence_across_tape_files()
{
    std::puts("-- add_file: last occurrence wins even across different tape files");
    Index idx = make_index();

    idx.add_file("video.mp4", 1000, "sha_v1", 0, 20, 1000, 0644);
    idx.add_file("video.mp4", 2000, "sha_v2", 2, 20, 2000, 0644);

    const Node *n = idx.resolve("video.mp4");
    CHECK(n != nullptr);
    CHECK(n->size == 2000);
    CHECK(n->sha256 == "sha_v2");
    CHECK(n->data_tape_file == 2);
}

static void test_directory_not_overwritten_by_file()
{
    std::puts("-- add_file: existing directory node is not overwritten");
    Index idx = make_index();
    idx.make_dir("somedir");

    // Attempt to add a file at the same path as a directory — should be ignored.
    idx.add_file("somedir", 10, "sha_x", 0, 20, 1000, 0644);

    const Node *n = idx.resolve("somedir");
    CHECK(n != nullptr);
    CHECK(n->is_dir);  // still a directory
}

static void test_sibling_files_unaffected()
{
    std::puts("-- add_file: updating one file does not disturb siblings");
    Index idx = make_index();
    idx.add_file("dir/a.txt", 1, "sha_a", 0, 20, 100, 0644);
    idx.add_file("dir/b.txt", 2, "sha_b", 0, 20, 200, 0644);

    // Update only a.txt
    idx.add_file("dir/a.txt", 99, "sha_a2", 0, 20, 999, 0644);

    const Node *a = idx.resolve("dir/a.txt");
    const Node *b = idx.resolve("dir/b.txt");
    CHECK(a != nullptr && a->size == 99);
    CHECK(b != nullptr && b->size == 2);
    CHECK(b->sha256 == "sha_b");
}

static void test_block_number_reset_on_overwrite()
{
    std::puts("-- add_file: block_number reset to -1 when overwriting existing entry");
    Index idx = make_index();

    idx.add_file("file.bin", 10, "sha_1", 0, 20, 1000, 0644);
    // Manually set block_number as tfsck verify would do
    Node *n = idx.resolve("file.bin");
    n->block_number = 12345;

    // Second occurrence on tape — block location is different
    idx.add_file("file.bin", 20, "sha_2", 0, 20, 2000, 0644);

    const Node *n2 = idx.resolve("file.bin");
    CHECK(n2 != nullptr);
    CHECK(n2->block_number == -1);  // reset so tfsck can fill in the correct value
}

static void test_release_flushed_staged()
{
    std::puts("-- release_flushed_staged: drops flushed, keeps in-flight, no-op on plain");
    Index idx = make_index();
    Node *a = idx.create_file("a"); a->staged = std::make_shared<Staged>(); a->staged_flushed = true;  // on closed tape file
    Node *b = idx.create_file("b"); b->staged = std::make_shared<Staged>(); b->staged_flushed = false; // released during sync
    Node *c = idx.create_file("c"); // no staged at all

    idx.release_flushed_staged();

    CHECK(!a->staged);                 // flushed -> released so read goes to tape
    CHECK(a->staged_flushed == false); // flag cleared
    CHECK(b->staged != nullptr);       // not yet flushed -> kept for next sync
    CHECK(!c->staged);                 // unaffected
}

// ── main ──────────────────────────────────────────────────────────────────────

int main()
{
    std::puts("=== test_index: add_file last-occurrence-wins ===");

    test_add_new_file();
    test_last_occurrence_wins();
    test_last_occurrence_across_tape_files();
    test_directory_not_overwritten_by_file();
    test_sibling_files_unaffected();
    test_block_number_reset_on_overwrite();
    test_release_flushed_staged();

    std::printf("=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
