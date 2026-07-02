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

#include "cli.hpp"
#include "index.hpp"
#include "tape.hpp"
#include "tar_io.hpp"
#include "raii.hpp"

#include <archive.h>

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
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
        struct MemberRec { std::string name, sha; uint64_t size; time_t mtime; mode_t mode;
                           int64_t block, offset; };
        std::vector<MemberRec> members;
        const bool ok = tape.scan_archive_detect_with_blocks(
            f, detected,
            [&](const std::string &name, int64_t block, int64_t offset,
                const std::string &sha, uint64_t size, time_t mtime, mode_t mode)
            {
                if (name == "manifest.json")
                    return; // always skip from data indexing (tapir or not)
                members.push_back({name, sha, size, mtime, mode, block, offset});
                if (verbose)
                    std::fprintf(stderr, "      %s  %llu  block %lld+%lld\n",
                                 sha.c_str(), static_cast<unsigned long long>(size),
                                 static_cast<long long>(block), static_cast<long long>(offset));
            },
            [&](const std::string &name, int64_t block, bool is_tapir_index)
            {
                if (is_tapir_index)
                    saw_manifest = true;
                else if (verbose)
                    std::fprintf(stderr, "    %-55s  block %lld\n",
                                 name.c_str(), static_cast<long long>(block));
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

        for (const auto &m : members)
        {
            idx.add_file(m.name, m.size, m.sha, f, bf, m.mtime, m.mode);
            idx.fill_block_location(m.name, f, m.block, m.offset);
        }
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
    // write_data is null, so the manifest lands at the tape file passed to make_manifest;
    // record it so every indexed data file points at this import's manifest.
    if (!tape.append(mbf, nullptr, [&](int manifest_tf)
                     { return idx.serialize(-1, mbf, manifest_tf); }, dtf))
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
//
// TODO: let the create/convert path populate the tape in one step — accept an
// optional `<file.tar>` or `-T <filelist>` (same inputs as `append`) so that
// `mktapir <device> file.tar` (or `-T list`) writes a fresh index AND streams
// the data as tape file 0, instead of requiring init-then-append. `do_append`
// already handles the no-existing-index case (it starts a fresh index when no
// manifest is found), so this is mostly wiring the init command's argv to the
// append data path when a tar/filelist is supplied, and keeping the empty-index
// behaviour when it is not.
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
        ok = tape.overwrite_from_start(mbf, [&]() { return idx.serialize(-1, mbf, 0); }); // manifest at file 0
    }
    else
    {
        int dtf = 0;
        ok = tape.append(mbf, nullptr, [&](int mp) { return idx.serialize(-1, mbf, mp); }, dtf);
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

// append: stream files into a new tape file at EOD and index them.
// tarpath: path to a .tar file to re-stream (empty when using filelist mode).
// filelist: path to a file containing one disk path per line, or "-" for stdin
//           (empty when using tarpath mode).
static int do_append(const std::string &dev, const std::string &tarpath,
                     const std::string &filelist, int bf, int mbf, bool verbose)
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

    struct AppendRec { std::string name, sha; uint64_t size; time_t mtime; mode_t mode;
                       int64_t block, offset; };
    std::vector<AppendRec> collected;
    int dtf = 0;
    const int bsize = bf * 512;

    auto on_member = [&](const std::string &name, int64_t block, int64_t offset,
                          const std::string &sha, uint64_t size, time_t mtime, mode_t mode)
    {
        collected.push_back({name, sha, size, mtime, mode, block, offset});
        if (verbose)
            std::fprintf(stderr, "    %s  %llu  %s  block %lld+%lld\n",
                         name.c_str(),
                         static_cast<unsigned long long>(size), sha.c_str(),
                         static_cast<long long>(block),
                         static_cast<long long>(offset));
    };

    auto make_manifest = [&](int data_tape_file) -> std::string
    {
        for (const auto &m : collected)
        {
            idx.add_file(m.name, m.size, m.sha, data_tape_file, bf, m.mtime, m.mode);
            if (m.block >= 0)
                idx.fill_block_location(m.name, data_tape_file, m.block, m.offset);
        }
        // Data was written at data_tape_file, so this manifest follows it at +1.
        return idx.serialize(-1, mbf, data_tape_file + 1);
    };

    bool ok;
    std::string source_desc;

    if (!filelist.empty())
    {
        // Read file paths from stdin or a named file, one per line.
        std::vector<std::string> paths;
        auto read_paths = [&](std::istream &is)
        {
            std::string line;
            while (std::getline(is, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!line.empty())
                    paths.push_back(std::move(line));
            }
        };
        if (filelist == "-")
        {
            read_paths(std::cin);
            source_desc = "stdin file list";
        }
        else
        {
            std::ifstream fs(filelist);
            if (!fs)
            {
                std::fprintf(stderr, "mktapir: cannot open file list %s\n", filelist.c_str());
                return 1;
            }
            read_paths(fs);
            source_desc = "file list " + filelist;
        }
        if (paths.empty())
        {
            std::fprintf(stderr, "mktapir: empty file list\n");
            return 1;
        }
        ok = tape.append(
            bf,
            [&](struct archive *out)
            { return tar_create_from_paths_with_blocks(paths, out, bsize, on_member); },
            make_manifest,
            dtf);
    }
    else
    {
        // Re-stream an existing tar file onto tape.
        ArchiveReadPtr in(archive_read_new());
        archive_read_support_filter_all(in.get()); // gzip/xz/zstd/etc.
        archive_read_support_format_all(in.get()); // V7/ustar/GNU/pax/star
        if (archive_read_open_filename(in.get(), tarpath.c_str(), 1 << 16) != ARCHIVE_OK)
        {
            std::fprintf(stderr, "mktapir: cannot open tar %s\n", tarpath.c_str());
            return 1;
        }
        source_desc = tarpath;
        ok = tape.append(
            bf,
            [&](struct archive *out)
            { return tar_copy_members_with_blocks(in.get(), out, bsize, on_member); },
            make_manifest,
            dtf);
    }

    if (!ok)
    {
        std::fprintf(stderr, "mktapir: append failed\n");
        return 1;
    }
    std::fprintf(stderr, "mktapir: appended %zu file(s) from %s as tape file %d; index updated\n",
                 collected.size(), source_desc.c_str(), dtf);
    std::fprintf(stderr, "mktapir: volume %s, write-generation %llu\n",
                 idx.volume_uuid().c_str(), static_cast<unsigned long long>(idx.latest_generation()));
    return 0;
}

static void usage(const char *argv0)
{
    std::fprintf(stderr,
                 "mktapir " PACKAGE_VERSION " — build/convert the tapir index on a tape\n"
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
                 "  %s append <tape-device> [<file.tar>] [-T <filelist>] [-b <block-factor>] [-m <manifest-bf>] [-v]\n"
                 "      Write a new tape file at EOD and add its contents to the index.\n"
                 "      <file.tar>: re-stream this tar; its members are indexed individually.\n"
                 "      No filename: read a list of disk paths from stdin, one per line,\n"
                 "                   and create a tar from those files.\n"
                 "      -T <filelist>: read disk paths from a file instead of stdin.\n"
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
        TapeOpts opts;
        opts.device = argv[1];
        bool force = false;
        auto extra = [&](const char *a, int &, int, char **) -> bool {
            if (std::strcmp(a, "--force") == 0) { force = true; return true; }
            return false;
        };
        if (!parse_tape_opts(argc, argv, 2, opts, argv[0], extra))
        {
            usage(argv[0]);
            return 2;
        }
        return do_init(opts.device, opts.mbf, force);
    }

    if (argc >= 3 && std::strcmp(argv[1], "import") == 0)
    {
        TapeOpts opts;
        opts.device = argv[2];
        opts.bf = 0; // 0 = auto-detect
        std::vector<int> files;
        auto extra = [&](const char *a, int &i, int argc, char **argv) -> bool {
            if ((std::strcmp(a, "-f") == 0 || std::strcmp(a, "--tape-file") == 0) && i + 1 < argc)
            {
                for (int n : parse_int_list(argv[++i]))
                    files.push_back(n);
                return true;
            }
            return false;
        };
        if (!parse_tape_opts(argc, argv, 3, opts, argv[0], extra))
        {
            usage(argv[0]);
            return 2;
        }
        if (opts.device.rfind("/dev/", 0) != 0) { usage(argv[0]); return 2; }
        return do_import(opts.device, files, opts.bf, opts.mbf, opts.verbose);
    }

    if (argc >= 3 && std::strcmp(argv[1], "append") == 0)
    {
        TapeOpts opts;
        opts.device = argv[2];
        std::string tarpath, filelist;

        // Optional positional tarpath: present when the next arg is not a flag.
        int start = 3;
        if (start < argc && argv[start][0] != '-')
            tarpath = argv[start++];

        auto extra = [&](const char *a, int &i, int argc, char **argv) -> bool {
            if ((std::strcmp(a, "-T") == 0 || std::strcmp(a, "--files-from") == 0) && i + 1 < argc)
            {
                filelist = argv[++i];
                return true;
            }
            return false;
        };
        if (!parse_tape_opts(argc, argv, start, opts, argv[0], extra))
        {
            usage(argv[0]);
            return 2;
        }
        if (opts.device.rfind("/dev/", 0) != 0) { usage(argv[0]); return 2; }
        if (!tarpath.empty() && !filelist.empty())
        {
            std::fprintf(stderr, "mktapir: a positional tar file and -T are mutually exclusive\n");
            return 2;
        }
        // No tar file and no -T <file>: default to reading paths from stdin.
        if (tarpath.empty() && filelist.empty())
            filelist = "-";
        return do_append(opts.device, tarpath, filelist, opts.bf, opts.mbf, opts.verbose);
    }

    usage(argv[0]);
    return 2;
}
