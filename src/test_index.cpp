// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

// test_index.cpp — unit tests for Index::add_file last-occurrence-wins semantics.
// Run via: make check

#include "index.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <memory>
#include <set>
#include <string>

using namespace tapir;
using json = nlohmann::json;

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

// Guards the manifest-persistence half of mtime preservation device-free: a real
// past mtime must survive serialize -> load, NOT collapse to 0 (which the FUSE
// layer would then render as the mount's current date — the "after import all
// mtimes were today" symptom). The hardware import/append tests cover the rest of
// the path (header -> add_file) and the actual FUSE getattr visibility.
static void test_mtime_survives_serialize_load()
{
    std::puts("-- mtime + fields survive a serialize -> load round-trip");
    Index idx = make_index();
    idx.add_file("a/keep.txt", 100, "sha_a", 0, 256, 1600000001, 0644);
    idx.add_file("b/exec.sh",  200, "sha_b", 0, 256, 1600000002, 0755);

    Index loaded;
    loaded.load(idx.serialize(-1, 256));
    const Node *a = loaded.resolve("a/keep.txt");
    const Node *b = loaded.resolve("b/exec.sh");
    CHECK(a != nullptr && b != nullptr);
    CHECK(a && a->mtime == 1600000001); // the field whose loss showed "current date"
    CHECK(a && a->size == 100);
    CHECK(a && a->sha256 == "sha_a");
    CHECK(b && b->mtime == 1600000002);
    CHECK(b && b->mode == 0755);

    // A genuinely-zero mtime must round-trip as 0 (FUSE then falls back to mount
    // time by design) — it must not be mangled into something else.
    Index z = make_index();
    z.add_file("c/no_mtime", 5, "sha_c", 0, 256, 0, 0644);
    Index zloaded;
    zloaded.load(z.serialize(-1, 256));
    const Node *c = zloaded.resolve("c/no_mtime");
    CHECK(c && c->mtime == 0);
}

// Guards the lost-update fix in WriterThread::sync. The manifest is snapshotted
// under the state lock, then written to tape WITHOUT it; mark_clean must only fire
// if no mutation raced that window. That rests on three Index invariants:
//   (1) every mutation advances version(),
//   (2) serialize() does NOT (so the writer's snapshot is stable across it),
//   (3) mark_clean() does NOT (so a clean index doesn't look "changed").
// If any regress, the writer would either never mark clean (stuck dirty) or
// silently drop a racing update from the on-tape manifest.
static void test_version_tracks_mutations_not_serialize()
{
    std::puts("-- version() advances on mutations, stable across serialize/mark_clean");
    Index idx = make_index();

    const uint64_t v0 = idx.version();
    idx.add_file("a.txt", 10, "sha_a", 0, 256, 1000, 0644);
    CHECK(idx.version() != v0);                 // add_file mutated
    const uint64_t v1 = idx.version();

    idx.create_file("b.txt");
    CHECK(idx.version() != v1);                 // create_file mutated
    const uint64_t v2 = idx.version();

    idx.mark_dirty();                           // explicit (t_release / read self-heal)
    CHECK(idx.version() != v2);
    const uint64_t v3 = idx.version();

    // serialize() is read-only w.r.t. version — the value captured right after it is
    // exactly what the writer's post-write guard compares against.
    const std::string manifest = idx.serialize(0, 256);
    CHECK(idx.version() == v3);
    const uint64_t snap = idx.version();

    // mark_clean clears dirty but must leave version untouched.
    idx.mark_clean();
    CHECK(!idx.dirty());
    CHECK(idx.version() == snap);

    // Emulate a FUSE op racing the unlocked tape write: it bumps version, so the
    // writer's `version() == snap` guard is false and the index is (correctly) kept
    // dirty rather than marked clean with the change dropped from tape.
    CHECK(idx.unlink_file("a.txt"));
    CHECK(idx.version() != snap);
    CHECK(idx.dirty());
}

// Guards the v2 manifest format + generations directory (the basis for the future
// .tapir/ snapshot view): the manifest root is an object holding a dedicated
// "generations" array of every manifest's location, not a per-archive field; each
// manifest records its own location; and the old v1 bare-array format still loads.
static void test_manifest_generations_directory()
{
    std::puts("-- serialize/load: v2 object + generations directory");
    // Import-style: data files 0,1,2 indexed by one manifest landing at file 3.
    Index idx = make_index();
    idx.add_file("a", 10, "sa", 0, 256, 1000, 0644);
    idx.add_file("b", 20, "sb", 1, 256, 1000, 0644);
    idx.add_file("c", 30, "sc", 2, 256, 1000, 0644);
    json doc = json::parse(idx.serialize(-1, 256, /*manifest_tape_file=*/3));
    CHECK(doc.is_object());
    CHECK(doc.value("format_version", 0) == 2);
    CHECK(doc.at("archives").is_array() && doc.at("archives").size() == 3);
    // location moved out of the per-archive header into the dedicated directory
    CHECK(!doc.at("archives").at(0).at(0).contains("manifest_tape_file"));
    CHECK(doc.at("generations").is_array() && doc.at("generations").size() == 1);
    CHECK(doc.at("generations").at(0).at("manifest_tape_file") == 3);

    // Load it back and re-serialize at a new location: the directory now lists both.
    Index loaded;
    loaded.load(doc.dump());
    CHECK(loaded.generations().size() == 1 && loaded.generations()[0].second == 3);
    json doc2 = json::parse(loaded.serialize(-1, 256, /*manifest_tape_file=*/5));
    std::set<int> locs;
    for (const auto &g : doc2.at("generations")) locs.insert(g.at("manifest_tape_file").get<int>());
    CHECK(locs.size() == 2 && locs.count(3) && locs.count(5));

    // v1 legacy (bare array with a per-archive manifest_tape_file) must still load,
    // and its directory is derived from that field.
    const char *v1 = R"([[{"data_tape_file":0,"manifest_tape_file":1,"write_generation":0,"block_factor":256},
                          {"path":"old","size":4,"mtime":1000}]])";
    Index legacy;
    legacy.load(v1);
    CHECK(legacy.resolve("old") != nullptr);
    CHECK(legacy.generations().size() == 1 && legacy.generations()[0].second == 1);
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
    test_mtime_survives_serialize_load();
    test_version_tracks_mutations_not_serialize();
    test_manifest_generations_directory();
    test_last_occurrence_wins();
    test_last_occurrence_across_tape_files();
    test_directory_not_overwritten_by_file();
    test_sibling_files_unaffected();
    test_block_number_reset_on_overwrite();
    test_release_flushed_staged();

    std::printf("=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
