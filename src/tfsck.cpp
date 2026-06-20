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

static int do_verify(const std::string &device, int bf, int mbf, bool verbose)
{
    Tape tape(device, mbf);
    tape.set_verbose(verbose);
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
    int missing_block_offsets = 0;
    for (const auto &f : index.flat())
    {
        expected[f.data_tape_file][f.path] = f.sha256;
        bf_of[f.data_tape_file] = f.block_factor ? f.block_factor : bf;
        if (f.block_number < 0 || f.block_offset < 0)
            ++missing_block_offsets;
    }

    std::printf("=== tfsck %s ===\n", device.c_str());
    if (!index.volume_uuid().empty())
        std::printf("  volume %s, write-generation %llu\n",
                    index.volume_uuid().c_str(),
                    static_cast<unsigned long long>(index.latest_generation()));
    if (missing_block_offsets > 0)
        std::printf("  NOTE: %d file(s) missing block offsets — will fill in during scan.\n",
                    missing_block_offsets);

    int failures = 0, orphans = 0, verified = 0, reindexed = 0;

    for (auto &[dtf, want] : expected)
    {
        std::set<std::string> seen;
        std::printf("  tape file %d (block factor %d): %zu file(s)\n",
                    dtf, bf_of[dtf], want.size());
        const bool ok = tape.scan_archive_with_blocks(
            dtf, bf_of[dtf],
            [&](const std::string &name, int64_t block, int64_t offset, const std::string &sha, uint64_t, time_t, mode_t)
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
                // Fill in the member's block + within-block offset from the scan so
                // later reads can seek straight to it.
                if (index.fill_block_location(name, dtf, block, offset))
                    ++reindexed;
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
            [&](const std::string &name, int64_t block, bool is_tapir_index)
            {
                if (verbose && !is_tapir_index)
                    std::printf("    %-55s  block %lld\n",
                                name.c_str(), static_cast<long long>(block));
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

    if (reindexed > 0)
    {
        int out_tf = 0;
        if (!tape.write_manifest_at_eod(index.serialize(0, bf), out_tf))
            std::fprintf(stderr, "tfsck: warning: failed to write updated manifest\n");
        else
            std::printf("--- filled in %d block offset(s), new manifest at tape file %d ---\n",
                        reindexed, out_tf);
    }

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

// ── rollback-previous ─────────────────────────────────────────────────────────

static int do_rollback_previous(const std::string &device, int mbf)
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

    // Collect all manifests in tape order.
    struct ManifestEntry { int tape_file; std::string json; uint64_t generation; size_t file_count; };
    std::vector<ManifestEntry> manifests;
    for (int f = 0; f < count; ++f)
    {
        std::string json;
        if (!tape.read_manifest_at(f, json))
            continue;
        Index idx;
        try { idx.load(json); } catch (...) { continue; }
        manifests.push_back({f, std::move(json), idx.latest_generation(), idx.flat().size()});
    }

    if (manifests.size() < 2)
    {
        std::fprintf(stderr, "tfsck: only %zu manifest(s) on tape — nothing to roll back to\n",
                     manifests.size());
        return 1;
    }

    const auto &prev = manifests[manifests.size() - 2];
    const auto &curr = manifests.back();
    std::fprintf(stderr,
                 "tfsck: rolling back from generation %llu (%zu file(s)) at tape file %d\n"
                 "       to generation %llu (%zu file(s)) at tape file %d\n",
                 static_cast<unsigned long long>(curr.generation), curr.file_count, curr.tape_file,
                 static_cast<unsigned long long>(prev.generation), prev.file_count, prev.tape_file);
    std::fprintf(stderr, "tfsck: writing rollback manifest at end of tape...\n");

    int out_tf = 0;
    if (!tape.append(mbf, nullptr, [&](int) { return prev.json; }, out_tf))
    {
        std::fprintf(stderr, "tfsck: failed to write rollback manifest\n");
        return 1;
    }

    Index target;
    target.load(prev.json);
    std::fprintf(stderr,
                 "tfsck: rollback complete — volume %s, generation %llu, %zu file(s), new tape file %d\n",
                 target.volume_uuid().c_str(),
                 static_cast<unsigned long long>(prev.generation),
                 prev.file_count, out_tf);
    return 0;
}

// ── upgrade-manifest ──────────────────────────────────────────────────────────

static int do_upgrade_manifest(const std::string &device, int mbf)
{
    Tape tape(device, mbf);

    bool full = false;
    if (tape.survey(full) < 0)
    {
        std::fprintf(stderr, "tfsck: cannot survey tape %s\n", device.c_str());
        return 1;
    }
    if (full)
    {
        std::fprintf(stderr, "tfsck: tape is full — cannot write upgraded manifest at EOD\n");
        return 1;
    }

    std::string json;
    if (!tape.read_latest_manifest_legacy(json))
    {
        std::fprintf(stderr, "tfsck: no manifest.json found at end of tape %s\n", device.c_str());
        return 1;
    }

    Index idx;
    try
    {
        idx.load(json);
    }
    catch (const std::exception &ex)
    {
        std::fprintf(stderr, "tfsck: manifest at end of tape is not valid tapir JSON: %s\n", ex.what());
        return 1;
    }

    std::fprintf(stderr,
                 "tfsck: found manifest — volume %s, generation %llu, %zu file(s)\n",
                 idx.volume_uuid().c_str(),
                 static_cast<unsigned long long>(idx.latest_generation()),
                 idx.flat().size());
    std::fprintf(stderr, "tfsck: writing upgraded manifest with PAX magic at end of tape...\n");

    // write_manifest_at_eod uses tar_write_member which adds the magic xattr.
    int out_tf = 0;
    if (!tape.write_manifest_at_eod(json, out_tf))
    {
        std::fprintf(stderr, "tfsck: failed to write upgraded manifest\n");
        return 1;
    }

    std::fprintf(stderr, "tfsck: upgrade complete — new manifest with PAX magic at tape file %d\n",
                 out_tf);
    return 0;
}

// ── main ─────────────────────────────────────────────────────────────────────

static void usage(const char *argv0)
{
    std::fprintf(stderr,
                 "tfsck " PACKAGE_VERSION " — verify and recovery tool for tapir tape archives\n"
                 "usage:\n"
                 "  %s <device> [-b N] [-m N] [-v]\n"
                 "      Verify all indexed files against their SHA-256 checksums.\n"
                 "      Missing per-member block offsets are filled in automatically\n"
                 "      and a new manifest is written at EOD if any were missing.\n"
                 "      -v: print every member name + block offset + sha256 + OK/FAIL.\n"
                 "  %s --list-generations <device> [-m N]\n"
                 "      List every tapir manifest on tape with generation, file count,\n"
                 "      and creation time.\n"
                 "  %s --rollback <device> [-m N]\n"
                 "      Roll back to the previous manifest (second-to-last on tape).\n"
                 "      Writes a copy of it as a new tape file at EOD.\n"
                 "  %s --rollback-to <generation> <device> [-m N]\n"
                 "      Write the manifest with the given write-generation as a new\n"
                 "      tape file at EOD, making it the new latest index. Old manifests\n"
                 "      remain on the WORM tape unchanged.\n"
                 "  %s --upgrade-manifest <device> [-m N]\n"
                 "      Read the manifest at the end of tape (even if it lacks the tapir\n"
                 "      PAX magic header) and write a new copy at EOD with the magic added.\n"
                 "      Use this to convert indexes written before PAX magic was introduced.\n"
                 "  -b N / --block-factor N         block factor for data reads (default 512)\n"
                 "  -m N / --manifest-block-factor  block factor for manifest writes (default 512)\n",
                 argv0, argv0, argv0, argv0, argv0);
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
            else if (device.empty() && !a.empty() && a[0] != '-')
                device = a;
            else if (!a.empty() && a[0] == '-')
            {
                std::fprintf(stderr, "tfsck: unknown option: %s\n", a.c_str());
                usage(argv[0]);
                return 2;
            }
        }
        if (device.empty())
        {
            usage(argv[0]);
            return 2;
        }
        return do_list_generations(device, mbf);
    }

    if (first == "--upgrade-manifest")
    {
        std::string device;
        int mbf = 512;
        for (int i = 2; i < argc; ++i)
        {
            const std::string a = argv[i];
            if ((a == "-m" || a == "--manifest-block-factor") && i + 1 < argc)
                mbf = std::atoi(argv[++i]);
            else if (device.empty() && !a.empty() && a[0] != '-')
                device = a;
            else if (!a.empty() && a[0] == '-')
            {
                std::fprintf(stderr, "tfsck: unknown option: %s\n", a.c_str());
                usage(argv[0]);
                return 2;
            }
        }
        if (device.empty())
        {
            usage(argv[0]);
            return 2;
        }
        return do_upgrade_manifest(device, mbf);
    }

    if (first == "--rollback")
    {
        std::string device;
        int mbf = 512;
        for (int i = 2; i < argc; ++i)
        {
            const std::string a = argv[i];
            if ((a == "-m" || a == "--manifest-block-factor") && i + 1 < argc)
                mbf = std::atoi(argv[++i]);
            else if (device.empty() && !a.empty() && a[0] != '-')
                device = a;
            else if (!a.empty() && a[0] == '-')
            {
                std::fprintf(stderr, "tfsck: unknown option: %s\n", a.c_str());
                usage(argv[0]);
                return 2;
            }
        }
        if (device.empty())
        {
            usage(argv[0]);
            return 2;
        }
        return do_rollback_previous(device, mbf);
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
            else if (device.empty() && !a.empty() && a[0] != '-')
                device = a;
            else if (!a.empty() && a[0] == '-')
            {
                std::fprintf(stderr, "tfsck: unknown option: %s\n", a.c_str());
                usage(argv[0]);
                return 2;
            }
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
    int bf = 512, mbf = 512;
    bool verbose = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "-v" || a == "--verbose")
            verbose = true;
        else if ((a == "-b" || a == "--block-factor") && i + 1 < argc)
            bf = std::atoi(argv[++i]);
        else if ((a == "-m" || a == "--manifest-block-factor") && i + 1 < argc)
            mbf = std::atoi(argv[++i]);
        else if (device.empty() && !a.empty() && a[0] != '-')
            device = a;
        else if (!a.empty() && a[0] == '-')
        {
            std::fprintf(stderr, "tfsck: unknown option: %s\n", a.c_str());
            usage(argv[0]);
            return 2;
        }
    }
    if (device.empty())
    {
        usage(argv[0]);
        return 2;
    }
    return do_verify(device, bf, mbf, verbose);
}
