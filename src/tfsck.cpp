// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

// tfsck.cpp — verify and recovery tool for tapir tape archives.
//
// Three modes:
//
//   tfsck <device> [-b N] [-v]
//     Verify all indexed files against their SHA-256 checksums on tape.
//     With -v: prints each member name on header read, then sha + OK/FAIL/ORPHAN.
//
//   tfsck --list-generations <device>
//     List every tapir manifest found on the tape with its generation number,
//     volume UUID, file count, and creation time. Useful before a rollback.
//
//   tfsck --rollback-to <generation> <device> [-m <manifest-bf>]
//     Find the manifest with the given write-generation and write it back as a
//     new tape file at EOD, making it the new latest index. All superseded
//     manifests remain on tape unchanged. This is the correct recovery path
//     when the index is corrupt or a bad write must be undone — analagous to
//     ltfsck's index-rollback mode. Refuses if the tape is full (EOT).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "index.hpp"
#include "tape.hpp"

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace tapir;

// ── verify ────────────────────────────────────────────────────────────────────

static int do_verify(const std::string &device, int bf, bool verbose)
{
    Tape tape(device, bf);
    std::string manifest_json;
    if (!tape.read_latest_manifest(manifest_json))
    {
        std::fprintf(stderr, "tfsck: could not read a manifest from %s\n", device.c_str());
        return 1;
    }
    Index index;
    try
    {
        index.load(manifest_json);
    }
    catch (const std::exception &ex)
    {
        std::fprintf(stderr, "tfsck: %s\n", ex.what());
        return 1;
    }

    std::map<int, std::map<std::string, std::string>> expected; // dtf -> {path: sha256}
    std::map<int, int> bf_of;
    for (const auto &f : index.flat())
    {
        expected[f.data_tape_file][f.path] = f.sha256;
        bf_of[f.data_tape_file] = f.block_factor ? f.block_factor : bf;
    }

    std::printf("=== tfsck %s ===\n", device.c_str());
    if (!index.volume_uuid().empty())
        std::printf("  volume %s, write-generation %llu\n",
                    index.volume_uuid().c_str(),
                    static_cast<unsigned long long>(index.latest_generation()));
    int failures = 0, orphans = 0, verified = 0;

    for (auto &[dtf, want] : expected)
    {
        std::set<std::string> seen;
        std::printf("  tape file %d (block factor %d): %zu file(s)\n",
                    dtf, bf_of[dtf], want.size());
        const bool ok = tape.scan_archive(
            dtf, bf_of[dtf],
            [&](const std::string &name, const std::string &sha, uint64_t, time_t)
            {
                seen.insert(name);
                auto it = want.find(name);
                if (it == want.end())
                {
                    ++orphans;
                    if (verbose)
                        std::printf("      %s  ORPHAN\n", sha.c_str());
                    return;
                }
                if (it->second.empty() || it->second == sha)
                {
                    ++verified;
                    if (verbose)
                        std::printf("      %s  OK\n", sha.c_str());
                }
                else
                {
                    ++failures;
                    if (verbose)
                        std::printf("      %s  FAIL  (expected %s)\n",
                                    sha.c_str(), it->second.c_str());
                    else
                        std::printf("    FAIL  %s\n"
                                    "      got      %s\n"
                                    "      expected %s\n",
                                    name.c_str(), sha.c_str(), it->second.c_str());
                }
            },
            [&](const std::string &name)
            {
                if (verbose)
                    std::printf("    %s\n", name.c_str());
            });
        if (!ok)
        {
            std::printf("    ERROR reading tape file %d\n", dtf);
            ++failures;
            continue;
        }
        for (const auto &[path, sha] : want)
        {
            (void)sha;
            if (!seen.count(path))
            {
                ++failures;
                std::printf("    FAIL  %s: missing from archive\n", path.c_str());
            }
        }
    }

    std::printf("--- %d verified, %d orphan(s) (deleted-but-retained), %d failure(s) ---\n",
                verified, orphans, failures);
    if (failures)
    {
        std::printf("=== tfsck: FAILED ===\n");
        return 1;
    }
    std::printf("=== tfsck: OK ===\n");
    return 0;
}

// ── list-generations ──────────────────────────────────────────────────────────

static int do_list_generations(const std::string &device, int mbf)
{
    Tape tape(device, mbf);
    bool full = false;
    const int count = tape.survey(full);
    if (count < 0)
    {
        std::fprintf(stderr, "tfsck: cannot survey tape %s\n", device.c_str());
        return 1;
    }

    std::printf("=== tfsck --list-generations %s ===\n", device.c_str());
    if (full)
        std::printf("  (tape is full — no room for further writes)\n");

    struct ManifestInfo
    {
        int tape_file;
        uint64_t generation;
        std::string volume_uuid;
        std::string created;
        size_t file_count;
    };
    std::vector<ManifestInfo> found;

    for (int f = 0; f < count; ++f)
    {
        std::string json;
        if (!tape.read_manifest_at(f, json))
            continue;
        Index idx;
        try
        {
            idx.load(json);
        }
        catch (const std::exception &ex)
        {
            std::printf("  tape file %2d: manifest (unreadable: %s)\n", f, ex.what());
            continue;
        }
        found.push_back({f, idx.latest_generation(), idx.volume_uuid(),
                         idx.created, idx.flat().size()});
    }

    if (found.empty())
    {
        std::printf("  (no tapir manifests found)\n");
        return 0;
    }

    for (std::size_t i = 0; i < found.size(); ++i)
    {
        const auto &m = found[i];
        const bool is_current = (i + 1 == found.size());
        std::printf("  tape file %2d: generation %llu  volume %s  %zu file(s)  %s%s\n",
                    m.tape_file,
                    static_cast<unsigned long long>(m.generation),
                    m.volume_uuid.empty() ? "(unknown)" : m.volume_uuid.c_str(),
                    m.file_count,
                    m.created.empty() ? "" : m.created.c_str(),
                    is_current ? "  ← current" : "");
    }
    return 0;
}

// ── rollback ──────────────────────────────────────────────────────────────────

static int do_rollback(const std::string &device, uint64_t target_gen, int mbf)
{
    Tape tape(device, mbf);

    bool full = false;
    const int count = tape.survey(full);
    if (count < 0)
    {
        std::fprintf(stderr, "tfsck: cannot survey tape %s\n", device.c_str());
        return 1;
    }
    if (full)
    {
        std::fprintf(stderr, "tfsck: tape is full — cannot write rollback manifest at EOD\n");
        return 1;
    }

    // Scan every tape file looking for the target generation.
    std::string found_json;
    int found_tf = -1;

    for (int f = 0; f < count; ++f)
    {
        std::string json;
        if (!tape.read_manifest_at(f, json))
            continue;
        Index idx;
        try
        {
            idx.load(json);
        }
        catch (...)
        {
            continue;
        }
        if (idx.latest_generation() == target_gen)
        {
            found_json = std::move(json);
            found_tf = f;
            // keep scanning — if the same generation appears more than once
            // (shouldn't happen) take the last occurrence (most recent copy)
        }
    }

    if (found_tf < 0)
    {
        std::fprintf(stderr, "tfsck: generation %llu not found on tape %s\n",
                     static_cast<unsigned long long>(target_gen), device.c_str());
        std::fprintf(stderr, "       run tfsck --list-generations to see what is available\n");
        return 1;
    }

    Index target;
    target.load(found_json); // already validated above
    std::fprintf(stderr,
                 "tfsck: found generation %llu at tape file %d — %s, %zu file(s)\n",
                 static_cast<unsigned long long>(target_gen), found_tf,
                 target.volume_uuid().c_str(), target.flat().size());
    std::fprintf(stderr, "tfsck: writing rollback manifest at end of tape...\n");

    // Write the old manifest verbatim as a new tape file at EOD. No data tape
    // file is written; all file references in the manifest still point to their
    // original tape files (which are still on the WORM tape unchanged).
    int dtf = 0;
    if (!tape.append(mbf, nullptr,
                     [&](int) { return found_json; },
                     dtf))
    {
        std::fprintf(stderr, "tfsck: failed to write rollback manifest\n");
        return 1;
    }

    std::fprintf(stderr,
                 "tfsck: rollback complete — volume %s, generation %llu, %zu file(s)\n",
                 target.volume_uuid().c_str(),
                 static_cast<unsigned long long>(target_gen),
                 target.flat().size());
    return 0;
}

// ── main ─────────────────────────────────────────────────────────────────────

static void usage(const char *argv0)
{
    std::fprintf(stderr,
                 "tfsck — verify and recovery tool for tapir tape archives\n"
                 "usage:\n"
                 "  %s <device> [-b N] [-v]\n"
                 "      Verify all indexed files against their SHA-256 checksums.\n"
                 "      -v: print every member name + sha256 + OK/FAIL as it streams.\n"
                 "  %s --list-generations <device> [-m N]\n"
                 "      List every tapir manifest on tape with generation, file count,\n"
                 "      and creation time.\n"
                 "  %s --rollback-to <generation> <device> [-m N]\n"
                 "      Write the manifest with the given write-generation as a new\n"
                 "      tape file at EOD, making it the new latest index. Old manifests\n"
                 "      remain on the WORM tape unchanged.\n"
                 "  -b N / --block-factor N        block factor for verify (default 512)\n"
                 "  -m N / --manifest-block-factor  block factor for manifest writes (default 512)\n",
                 argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
    std::setlocale(LC_ALL, "");

    if (argc < 2)
    {
        usage(argv[0]);
        return 2;
    }

    // Detect mode from first option-like argument.
    const std::string first = argv[1];

    if (first == "--list-generations")
    {
        std::string device;
        int mbf = 512;
        for (int i = 2; i < argc; ++i)
        {
            const std::string a = argv[i];
            if ((a == "-m" || a == "--manifest-block-factor") && i + 1 < argc)
                mbf = std::atoi(argv[++i]);
            else if (device.empty() && a[0] != '-')
                device = a;
        }
        if (device.empty())
        {
            usage(argv[0]);
            return 2;
        }
        return do_list_generations(device, mbf);
    }

    if (first == "--rollback-to")
    {
        if (argc < 4)
        {
            usage(argv[0]);
            return 2;
        }
        const uint64_t target_gen = static_cast<uint64_t>(std::atoll(argv[2]));
        std::string device;
        int mbf = 512;
        for (int i = 3; i < argc; ++i)
        {
            const std::string a = argv[i];
            if ((a == "-m" || a == "--manifest-block-factor") && i + 1 < argc)
                mbf = std::atoi(argv[++i]);
            else if (device.empty() && a[0] != '-')
                device = a;
        }
        if (device.empty())
        {
            usage(argv[0]);
            return 2;
        }
        return do_rollback(device, target_gen, mbf);
    }

    // Default: verify mode. Device is first non-flag argument.
    std::string device;
    int bf = 512;
    bool verbose = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "-v" || a == "--verbose")
            verbose = true;
        else if ((a == "-b" || a == "--block-factor") && i + 1 < argc)
            bf = std::atoi(argv[++i]);
        else if (device.empty() && !a.empty() && a[0] != '-')
            device = a;
    }
    if (device.empty())
    {
        usage(argv[0]);
        return 2;
    }
    return do_verify(device, bf, verbose);
}
