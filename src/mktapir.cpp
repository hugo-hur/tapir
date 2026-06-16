// mktapir.cpp — build / convert the tapir index on a tape (cf. mkltfs).
//
//   mktapir import <device> -f <tape-file> -b <block-factor> [-m <manifest-bf>]
//
// Scans an existing tar at tape file N (read with blocking factor B), computes
// each member's SHA-256, and adds them to the index recording (N, B). This is the
// building block for converting pre-existing, non-tapir tapes — whose data tars
// may each use a different blocking factor — into tapir-compatible ones: run it
// once per data tape file with that file's -b. An updated manifest is written at
// the end of the tape; existing data is never rewritten.

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

static int do_import(const std::string &dev, int tape_file, int bf_hint, int mbf)
{
    Tape tape(dev, mbf);

    // Auto-detect the data archive's block size; -b (bf_hint) is only an override.
    int bf = bf_hint;
    const int probed = tape.probe_block_size(tape_file);
    if (probed > 0)
    {
        const int probed_bf = (probed + 511) / 512; // round up so the read buffer ≥ block
        if (bf_hint > 0 && bf_hint != probed_bf)
            std::fprintf(stderr, "mktapir: WARNING: -b %d disagrees with detected %d bytes "
                                 "(block factor %d); using detected\n",
                         bf_hint, probed, probed_bf);
        bf = probed_bf;
        std::fprintf(stderr, "mktapir: tape file %d block size = %d bytes (block factor %d)\n",
                     tape_file, probed, bf);
    }
    else if (bf_hint > 0)
    {
        std::fprintf(stderr, "mktapir: could not auto-detect block size; using -b %d\n", bf_hint);
    }
    else
    {
        std::fprintf(stderr, "mktapir: could not detect block size at tape file %d; pass -b\n", tape_file);
        return 1;
    }

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

    int added = 0;
    const bool ok = tape.scan_archive(
        tape_file, bf,
        [&](const std::string &name, const std::string &sha, uint64_t size)
        {
            idx.add_file(name, size, sha, tape_file, bf);
            ++added;
        });
    if (!ok)
    {
        std::fprintf(stderr, "mktapir: failed to scan tape file %d (block factor %d)\n", tape_file, bf);
        return 1;
    }
    std::fprintf(stderr, "mktapir: indexed %d member(s) from tape file %d (block factor %d)\n",
                 added, tape_file, bf);

    int dtf = 0;
    if (!tape.append(mbf, nullptr, [&](int)
                     { return idx.serialize(-1, mbf); }, dtf))
    {
        std::fprintf(stderr, "mktapir: failed to write updated manifest\n");
        return 1;
    }
    std::fprintf(stderr, "mktapir: wrote updated index to end of tape\n");
    return 0;
}

// append: re-stream a tar from disk into a new tape file at EOD and index it.
static int do_append(const std::string &dev, const std::string &tarpath, int bf, int mbf)
{
    Tape tape(dev, mbf);
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
    archive_read_support_format_tar(in.get());
    archive_read_support_filter_all(in.get()); // tolerate .tar.gz/.xz etc.
    if (archive_read_open_filename(in.get(), tarpath.c_str(), 1 << 16) != ARCHIVE_OK)
    {
        std::fprintf(stderr, "mktapir: cannot open tar %s\n", tarpath.c_str());
        return 1;
    }

    std::vector<std::tuple<std::string, std::string, uint64_t>> collected;
    int dtf = 0;
    const bool ok = tape.append(
        bf,
        [&](struct archive *out)
        {
            return tar_copy_members(in.get(), out,
                                    [&](const std::string &name, const std::string &sha, uint64_t size)
                                    {
                                        collected.emplace_back(name, sha, size);
                                    });
        },
        [&](int data_tape_file)
        {
            for (auto &[name, sha, size] : collected)
                idx.add_file(name, size, sha, data_tape_file, bf);
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
    return 0;
}

static void usage(const char *argv0)
{
    std::fprintf(stderr,
                 "mktapir — build/convert the tapir index on a tape\n"
                 "usage:\n"
                 "  %s import <tape-device> -f <tape-file> [-b <block-factor>] [-m <manifest-bf>]\n"
                 "      Scan the existing tar at tape file N and add its members to the index,\n"
                 "      then write an updated manifest at end of tape. The block size is\n"
                 "      auto-detected; pass -b only to override. Run once per data tape file\n"
                 "      to convert a non-tapir tape.\n"
                 "  %s append <tape-device> <file.tar> [-b <block-factor>] [-m <manifest-bf>]\n"
                 "      Re-stream a tar from disk into a new tape file at EOD and add its\n"
                 "      contents to the index.\n",
                 argv0, argv0);
}

int main(int argc, char **argv)
{
    std::setlocale(LC_ALL, "");

    if (argc >= 3 && std::strcmp(argv[1], "import") == 0)
    {
        const std::string dev = argv[2];
        int tape_file = -1, bf = 0 /* 0 = auto-detect */, mbf = 512;
        for (int i = 3; i < argc; ++i)
        {
            const std::string a = argv[i];
            if ((a == "-f" || a == "--tape-file") && i + 1 < argc)
                tape_file = std::atoi(argv[++i]);
            else if ((a == "-b" || a == "--block-factor") && i + 1 < argc)
                bf = std::atoi(argv[++i]);
            else if ((a == "-m" || a == "--manifest-block-factor") && i + 1 < argc)
                mbf = std::atoi(argv[++i]);
        }
        if (dev.rfind("/dev/", 0) != 0 || tape_file < 0)
        {
            usage(argv[0]);
            return 2;
        }
        return do_import(dev, tape_file, bf, mbf);
    }

    if (argc >= 4 && std::strcmp(argv[1], "append") == 0)
    {
        const std::string dev = argv[2], tarpath = argv[3];
        int bf = 512, mbf = 512;
        for (int i = 4; i < argc; ++i)
        {
            const std::string a = argv[i];
            if ((a == "-b" || a == "--block-factor") && i + 1 < argc)
                bf = std::atoi(argv[++i]);
            else if ((a == "-m" || a == "--manifest-block-factor") && i + 1 < argc)
                mbf = std::atoi(argv[++i]);
        }
        if (dev.rfind("/dev/", 0) != 0)
        {
            usage(argv[0]);
            return 2;
        }
        return do_append(dev, tarpath, bf, mbf);
    }

    usage(argv[0]);
    return 2;
}
