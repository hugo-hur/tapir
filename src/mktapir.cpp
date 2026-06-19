// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

// mktapir.cpp — build / convert the tapir index on a tape (cf. mkltfs).
//
//   mktapir import <device> [-f <tape-files>] [-b <block-factor>] [-m <manifest-bf>]
//
// Scans existing tar tape files, computes each member's SHA-256, and adds them to
// the index recording each archive's tape file + (auto-detected) block factor.
// With no -f it scans the whole tape (every data file; manifest files skipped),
// converting a pre-existing, non-tapir tape — whose data tars may each use a
// different blocking factor — in a single command. An updated manifest is written
// at end of tape; existing data is never rewritten.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "index.hpp"
#include "tape.hpp"
#include "tar_io.hpp"
#include "raii.hpp"

#include <archive.h>

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

using namespace tapir;

// import: index existing tar tape files. `files` empty → every data tape file.
// Block size is auto-detected per file; bf_hint (-b) is only a fallback/override.
static int do_import(const std::string &dev, const std::vector<int> &files, int bf_hint, int mbf, bool verbose)
{
    Tape tape(dev, mbf);
    tape.set_verbose(verbose); // narrate tape positioning/reads

    // One forward survey: how many tape files, and is the tape full?
    bool full = false;
    const int count = tape.survey(full);
    if (count < 0)
    {
        std::fprintf(stderr, "mktapir: cannot read tape %s\n", dev.c_str());
        return 1;
    }
    if (full)
    {
        std::fprintf(stderr, "mktapir: tape is full — no room to write the index. A tar that\n"
                             "         spans multiple tapes is not supported; refusing.\n");
        return 1;
    }

    // An explicit -f list adds to the existing index; a full scan (no -f) is only
    // for tapes that have never had a tapir index written — if one already exists,
    // refuse: a rescan would recover index-deleted files and mis-index overwritten
    // files (first occurrence wins, not latest). Use tfsck for recovery instead.
    Index idx;
    if (!files.empty())
    {
        std::string manifest;
        if (tape.read_latest_manifest(manifest))
        {
            try
            {
                idx.load(manifest);
                std::fprintf(stderr, "mktapir: loaded existing index (%zu file(s))\n", idx.flat().size());
            }
            catch (const std::exception &ex)
            {
                std::fprintf(stderr, "mktapir: existing manifest unreadable (%s); starting fresh\n", ex.what());
            }
        }
        else
        {
            std::fprintf(stderr, "mktapir: no existing manifest; starting fresh\n");
        }
    }
    else
    {
        std::string existing;
        if (count > 0 && tape.read_manifest_at(count - 1, existing))
        {
            std::fprintf(stderr,
                         "mktapir: tape already has a tapir index — refusing full rescan.\n"
                         "         A rescan would resurrect index-deleted files whose data is still\n"
                         "         on tape. Use -f <tape-files> to add specific new tape files to the\n"
                         "         index, or use tfsck for index recovery and rollback.\n");
            return 1;
        }
        std::fprintf(stderr, "mktapir: building a new index from all data on the tape\n");
    }

    // Default target set: every tape file. Manifest tape files are skipped
    // automatically (their sole member is manifest.json).
    std::vector<int> targets = files;
    if (targets.empty())
        for (int f = 0; f < count; ++f)
            targets.push_back(f);

    int total = 0, skipped = 0, incomplete = 0;
    for (const int f : targets)
    {
        if (f >= count)
        {
            std::fprintf(stderr, "mktapir: tape file %d is past end-of-data (%d files); skipping\n", f, count);
            continue;
        }
        int detected = 0;
        bool saw_manifest = false;
        std::vector<std::tuple<std::string, std::string, uint64_t, time_t, mode_t>> members;
        const bool ok = tape.scan_archive_detect(
            f, detected,
            [&](const std::string &name, const std::string &sha, uint64_t size, time_t mtime, mode_t mode)
            {
                if (name == "manifest.json")
                    return; // always skip from data indexing (tapir or not)
                members.emplace_back(name, sha, size, mtime, mode);
                if (verbose)
                    std::fprintf(stderr, "      %s  %llu\n", sha.c_str(), static_cast<unsigned long long>(size));
            },
            [&](const std::string &name, bool is_tapir_index)
            {
                if (is_tapir_index)
                    saw_manifest = true;
                else if (verbose)
                    std::fprintf(stderr, "    %s\n", name.c_str());
            });
        if (saw_manifest)
        {
            std::fprintf(stderr,
                         "mktapir: tape file %d is a tapir index — refusing to import over an existing index.\n"
                         "         Use -f <tape-files> to add specific new data tape files to the index,\n"
                         "         or use tfsck for index recovery and rollback.\n", f);
            return 1;
        }
        if (!ok && members.empty())
        {
            // Nothing parsed: no valid tar header at the file's start — the tail of
            // an archive spanned from a previous tape, or corrupt. Disregard it.
            std::fprintf(stderr,
                         "mktapir: WARNING: tape file %d has no valid tar header at its start\n"
                         "         (a continuation from another tape, or corrupt); skipping it.\n",
                         f);
            ++skipped;
            continue;
        }
        if (!ok)
        {
            // Parsed some complete members, then hit an error: the archive is
            // incomplete — truncated, or it continues on another tape. Keep the
            // complete members; the cut-off trailing one is disregarded.
            std::fprintf(stderr,
                         "mktapir: WARNING: tape file %d is incomplete (truncated, or it continues on\n"
                         "         another tape) — indexing the %zu complete member(s) before the cut.\n",
                         f, members.size());
            ++incomplete;
        }
        if (members.empty())
            continue; // empty or unrecognised tape file — skip silently

        const int detected_bf = detected > 0 ? (detected + 511) / 512 : 0;
        const int bf = detected_bf > 0 ? detected_bf : bf_hint;
        if (bf <= 0)
        {
            std::fprintf(stderr, "mktapir: could not detect block size at tape file %d; pass -b\n", f);
            continue;
        }
        if (bf_hint > 0 && detected_bf > 0 && bf_hint != detected_bf)
            std::fprintf(stderr, "mktapir: WARNING: -b %d disagrees with detected block factor %d "
                                 "at tape file %d; using detected\n",
                         bf_hint, detected_bf, f);

        for (const auto &[name, sha, size, mtime, mode] : members)
            idx.add_file(name, size, sha, f, bf, mtime, mode);
        total += static_cast<int>(members.size());
        std::fprintf(stderr, "mktapir: tape file %d: %zu file(s), block size %d bytes (factor %d)\n",
                     f, members.size(), detected, bf);
    }

    if (skipped || incomplete)
        std::fprintf(stderr, "mktapir: %d tape file(s) skipped, %d incomplete\n", skipped, incomplete);

    if (total == 0)
    {
        std::fprintf(stderr, "mktapir: nothing to index\n");
        return 1;
    }

    int dtf = 0;
    if (!tape.append(mbf, nullptr, [&](int)
                     { return idx.serialize(-1, mbf); }, dtf))
    {
        std::fprintf(stderr, "mktapir: failed to write updated manifest\n");
        return 1;
    }
    std::fprintf(stderr, "mktapir: indexed %d file(s) across %zu tape file(s); wrote index at end of tape\n",
                 total, targets.size());
    std::fprintf(stderr, "mktapir: volume %s, write-generation %llu\n",
                 idx.volume_uuid().c_str(), static_cast<unsigned long long>(idx.latest_generation()));
    return 0;
}

// init: write a fresh empty tapir index on a blank (or force-overwritten) tape.
static int do_init(const std::string &dev, int mbf, bool force)
{
    Tape tape(dev, mbf);

    bool full = false;
    const int count = tape.survey(full);
    if (count < 0)
    {
        std::fprintf(stderr, "mktapir: cannot read tape %s\n", dev.c_str());
        return 1;
    }
    if (full)
    {
        std::fprintf(stderr, "mktapir: tape is full — cannot write index\n");
        return 1;
    }

    if (count > 0 && !force)
    {
        std::fprintf(stderr,
                     "mktapir: tape already has %d file(s) — refusing to write a blank index.\n"
                     "         Use --force to write a new empty index at start of tape anyway.\n"
                     "         In this case existing data on tape will be lost.\n",
                     count);
        return 1;
    }

    Index idx;
    bool ok;
    if (count > 0)
    {
        std::fprintf(stderr,
                     "mktapir: WARNING: tape has %d existing file(s); rewinding and writing\n"
                     "         blank index at start of tape. Existing data will be inaccessible\n"
                     "         (on WORM tapes the drive will reject this write).\n",
                     count);
        ok = tape.overwrite_from_start(mbf, [&]() { return idx.serialize(-1, mbf); });
    }
    else
    {
        int dtf = 0;
        ok = tape.append(mbf, nullptr, [&](int) { return idx.serialize(-1, mbf); }, dtf);
    }
    if (!ok)
    {
        std::fprintf(stderr, "mktapir: failed to write index\n");
        return 1;
    }
    std::fprintf(stderr, "mktapir: initialised tape %s — volume %s, generation %llu\n",
                 dev.c_str(),
                 idx.volume_uuid().c_str(),
                 static_cast<unsigned long long>(idx.latest_generation()));
    return 0;
}

// append: re-stream a tar from disk into a new tape file at EOD and index it.
static int do_append(const std::string &dev, const std::string &tarpath, int bf, int mbf, bool verbose)
{
    Tape tape(dev, mbf);
    tape.set_verbose(verbose); // narrate tape positioning/reads
    Index idx;

    std::string manifest;
    if (tape.read_latest_manifest(manifest))
    {
        try
        {
            idx.load(manifest);
            std::fprintf(stderr, "mktapir: loaded existing index (%zu file(s))\n", idx.flat().size());
        }
        catch (const std::exception &ex)
        {
            std::fprintf(stderr, "mktapir: existing manifest unreadable (%s); starting fresh\n", ex.what());
        }
    }
    else
    {
        std::fprintf(stderr, "mktapir: no existing manifest; starting a new index\n");
    }

    ArchiveReadPtr in(archive_read_new());
    archive_read_support_filter_all(in.get()); // gzip/xz/zstd/etc.
    archive_read_support_format_all(in.get()); // V7/ustar/GNU/pax/star
    if (archive_read_open_filename(in.get(), tarpath.c_str(), 1 << 16) != ARCHIVE_OK)
    {
        std::fprintf(stderr, "mktapir: cannot open tar %s\n", tarpath.c_str());
        return 1;
    }

    std::vector<std::tuple<std::string, std::string, uint64_t, time_t, mode_t>> collected;
    int dtf = 0;
    const bool ok = tape.append(
        bf,
        [&](struct archive *out)
        {
            return tar_copy_members(in.get(), out,
                                    [&](const std::string &name, const std::string &sha,
                                        uint64_t size, time_t mtime, mode_t mode)
                                    {
                                        collected.emplace_back(name, sha, size, mtime, mode);
                                        if (verbose)
                                            std::fprintf(stderr, "    %s  %llu  %s\n", name.c_str(),
                                                         static_cast<unsigned long long>(size), sha.c_str());
                                    });
        },
        [&](int data_tape_file)
        {
            for (auto &[name, sha, size, mtime, mode] : collected)
                idx.add_file(name, size, sha, data_tape_file, bf, mtime, mode);
            return idx.serialize(-1, mbf);
        },
        dtf);
    if (!ok)
    {
        std::fprintf(stderr, "mktapir: append failed\n");
        return 1;
    }
    std::fprintf(stderr, "mktapir: appended %zu file(s) from %s as tape file %d; index updated\n",
                 collected.size(), tarpath.c_str(), dtf);
    std::fprintf(stderr, "mktapir: volume %s, write-generation %llu\n",
                 idx.volume_uuid().c_str(), static_cast<unsigned long long>(idx.latest_generation()));
    return 0;
}

static void usage(const char *argv0)
{
    std::fprintf(stderr,
                 "mktapir — build/convert the tapir index on a tape\n"
                 "usage:\n"
                 "  %s <tape-device> [-m <manifest-bf>] [--force]\n"
                 "      Write a fresh empty tapir index on a blank tape. Refuses if the tape\n"
                 "      already has files unless --force is given; --force rewinds to the\n"
                 "      start of tape and overwrites with a blank index (on WORM tapes the\n"
                 "      drive will reject this if data already exists).\n"
                 "  %s import <tape-device> [-f <tape-files>] [-b <block-factor>] [-m <manifest-bf>] [-v]\n"
                 "      Index existing tar tape files and write an updated manifest at end of\n"
                 "      tape. With no -f, scans every data tape file (manifest files are\n"
                 "      skipped); -f takes a comma-separated list (e.g. -f 0,2,5). The block\n"
                 "      size is auto-detected per file; -b is only a fallback/override.\n"
                 "  %s append <tape-device> <file.tar> [-b <block-factor>] [-m <manifest-bf>] [-v]\n"
                 "      Re-stream a tar from disk into a new tape file at EOD and add its\n"
                 "      contents to the index.\n"
                 "  -v / --verbose   log tape positioning and, per member, '<name> <size> <sha256>'\n"
                 "                   as each member's SHA-256 finishes.\n",
                 argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
    std::setlocale(LC_ALL, "");

    // Default command: mktapir <device> [--force] [-m N]
    // Detected when the first argument looks like a device path.
    if (argc >= 2 && std::string(argv[1]).rfind("/dev/", 0) == 0)
    {
        const std::string dev = argv[1];
        int mbf = 512;
        bool force = false;
        for (int i = 2; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--force")
                force = true;
            else if ((a == "-m" || a == "--manifest-block-factor") && i + 1 < argc)
                mbf = std::atoi(argv[++i]);
        }
        return do_init(dev, mbf, force);
    }

    if (argc >= 3 && std::strcmp(argv[1], "import") == 0)
    {
        const std::string dev = argv[2];
        std::vector<int> files; // empty → all data tape files
        int bf = 0 /* 0 = auto-detect */, mbf = 512;
        bool verbose = false;
        for (int i = 3; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "-v" || a == "--verbose")
                verbose = true;
            else if ((a == "-f" || a == "--tape-file") && i + 1 < argc)
            {
                // comma-separated list, e.g. -f 0,2,5 (repeatable)
                const std::string v = argv[++i];
                for (std::size_t p = 0; p < v.size();)
                {
                    const std::size_t c = v.find(',', p);
                    const std::string tok = v.substr(p, c == std::string::npos ? c : c - p);
                    if (!tok.empty())
                        files.push_back(std::atoi(tok.c_str()));
                    if (c == std::string::npos)
                        break;
                    p = c + 1;
                }
            }
            else if ((a == "-b" || a == "--block-factor") && i + 1 < argc)
                bf = std::atoi(argv[++i]);
            else if ((a == "-m" || a == "--manifest-block-factor") && i + 1 < argc)
                mbf = std::atoi(argv[++i]);
        }
        if (dev.rfind("/dev/", 0) != 0)
        {
            usage(argv[0]);
            return 2;
        }
        return do_import(dev, files, bf, mbf, verbose);
    }

    if (argc >= 4 && std::strcmp(argv[1], "append") == 0)
    {
        const std::string dev = argv[2], tarpath = argv[3];
        int bf = 512, mbf = 512;
        bool verbose = false;
        for (int i = 4; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "-v" || a == "--verbose")
                verbose = true;
            else if ((a == "-b" || a == "--block-factor") && i + 1 < argc)
                bf = std::atoi(argv[++i]);
            else if ((a == "-m" || a == "--manifest-block-factor") && i + 1 < argc)
                mbf = std::atoi(argv[++i]);
        }
        if (dev.rfind("/dev/", 0) != 0)
        {
            usage(argv[0]);
            return 2;
        }
        return do_append(dev, tarpath, bf, mbf, verbose);
    }

    usage(argv[0]);
    return 2;
}
