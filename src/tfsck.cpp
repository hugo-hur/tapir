// tfsck.cpp — verify a tapir/ltfs_to_tar tape against its manifest.
//
// Reads the latest manifest, then streams each data tape file and recomputes
// every member's SHA-256, comparing against the index. Unlike a naive verifier,
// a member present on tape but absent from the index is reported as an *orphan*
// (expected after an index-only delete), not a failure. Exits non-zero only on
// real problems: checksum mismatch or a manifest entry whose data is missing.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "index.hpp"
#include "tape.hpp"

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>

using namespace tapir;

int main(int argc, char** argv) {
    std::setlocale(LC_ALL, "");

    std::string device;
    int bf = 512;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if ((a == "-b" || a == "--block-factor") && i + 1 < argc) { bf = std::atoi(argv[++i]); continue; }
        if (device.empty() && !a.empty() && a[0] != '-') device = a;
    }
    if (device.empty()) {
        std::fprintf(stderr, "tfsck — verify a tar tape archive against its manifest\n"
                             "usage: %s <tape-device> [-b N]\n", argv[0]);
        return 2;
    }

    Tape tape(device, bf);
    std::string manifest_json;
    if (!tape.read_latest_manifest(manifest_json)) {
        std::fprintf(stderr, "tfsck: could not read a manifest from %s\n", device.c_str());
        return 1;
    }
    Index index;
    try {
        index.load(manifest_json);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "tfsck: %s\n", ex.what());
        return 1;
    }

    // Group expected files by data tape file.
    std::map<int, std::map<std::string, std::string>> expected;   // dtf -> {path: sha256}
    std::map<int, int>                                bf_of;       // dtf -> block_factor
    for (const auto& f : index.flat()) {
        expected[f.data_tape_file][f.path] = f.sha256;
        bf_of[f.data_tape_file] = f.block_factor ? f.block_factor : bf;
    }

    std::printf("=== tfsck %s ===\n", device.c_str());
    int failures = 0, orphans = 0, verified = 0;

    for (auto& [dtf, want] : expected) {
        std::set<std::string> seen;
        std::printf("  tape file %d (block factor %d): %zu file(s)\n", dtf, bf_of[dtf], want.size());
        const bool ok = tape.scan_archive(
            dtf, bf_of[dtf],
            [&](const std::string& name, const std::string& sha, uint64_t) {
                seen.insert(name);
                auto it = want.find(name);
                if (it == want.end()) {
                    ++orphans;                              // present on tape, not indexed (a prior delete)
                    return;
                }
                if (it->second.empty() || it->second == sha) {
                    ++verified;
                } else {
                    ++failures;
                    std::printf("    FAIL  %s: sha256 mismatch\n", name.c_str());
                }
            });
        if (!ok) {
            std::printf("    ERROR reading tape file %d\n", dtf);
            ++failures;
            continue;
        }
        for (const auto& [path, sha] : want) {
            (void)sha;
            if (!seen.count(path)) {
                ++failures;
                std::printf("    FAIL  %s: missing from archive\n", path.c_str());
            }
        }
    }

    std::printf("--- %d verified, %d orphan(s) (deleted-but-retained), %d failure(s) ---\n",
                verified, orphans, failures);
    if (failures) {
        std::printf("=== tfsck: FAILED ===\n");
        return 1;
    }
    std::printf("=== tfsck: OK ===\n");
    return 0;
}
