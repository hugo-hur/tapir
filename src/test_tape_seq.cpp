// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

// test_tape_seq.cpp — hardware test: write a multi-member archive, then read it
// back SEQUENTIALLY (scan_archive_with_blocks, no per-member seek) and verify
// every member's sha256. This proves the on-tape data is intact and that a full
// streaming read round-trips — independent of the per-member seek path.
//
// See test_tape_common.hpp for the TAPIR_TEST_DEVICE parameterisation / SKIP.

#include "test_tape_common.hpp"

int main()
{
    int bf;
    const char *dev = tape_test_device(bf);
    if (!dev) { std::printf("SKIP: set TAPIR_TEST_DEVICE=<scratch no-rewind tape> to run\n"); return 77; }
    std::printf("=== test_tape_seq on %s (block factor %d = %d KiB) ===\n", dev, bf, bf * 512 / 1024);

    std::vector<Member> ms = sample_members();
    int dtf = -1;
    if (!write_archive(dev, bf, ms, dtf)) { check(false, "append: multi-member data + manifest at EOD"); return finish(); }
    check(true, "append: multi-member data + manifest at EOD");
    std::printf("  (data archive at tape file %d)\n", dtf);

    // Fresh Tape: reads happen in a separate process in real use (mktapir/tfsck),
    // not reusing the writer's position state.
    Tape rtape(dev, bf);
    int seen = 0;
    bool scan = rtape.scan_archive_with_blocks(
        dtf, bf,
        [&](const std::string &name, int64_t, int64_t, const std::string &sha, uint64_t, time_t, mode_t) {
            for (auto &m : ms)
                if (m.name == name) { ++seen; check(sha == m.sha, (std::string("sequential sha: ") + name).c_str()); }
        },
        {});
    check(scan, "scan_archive_with_blocks: sequential read");
    check(seen == static_cast<int>(ms.size()), "sequential scan saw all members");

    return finish();
}
