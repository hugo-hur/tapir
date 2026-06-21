// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

// test_tape_seek.cpp — hardware test: write a multi-member archive, then read
// each member back by RANDOM-ACCESS seek (read_member with the per-member block)
// and verify its content. This exercises Tape::read_member / seek_to — the
// per-member positioning path used by the FUSE mount for fast reads.
//
// A sequential scan is run first only to obtain each member's block number;
// the assertions are on the per-member seek reads. A final case feeds read_member
// a deliberately WRONG offset to exercise the self-heal: the fast positional read
// must fail, read_member must fall back to a name-scan from the tape file's start,
// return the correct content, AND report the corrected (block, offset) — the value
// the FUSE layer writes back into the index to fix a stale position.
//
// See test_tape_common.hpp for the TAPIR_TEST_DEVICE parameterisation / SKIP.

#include "test_tape_common.hpp"

int main()
{
    int bf;
    const char *dev = tape_test_device(bf);
    if (!dev) { std::printf("SKIP: set TAPIR_TEST_DEVICE=<scratch no-rewind tape> to run\n"); return 77; }
    std::printf("=== test_tape_seek on %s (block factor %d = %d KiB) ===\n", dev, bf, bf * 512 / 1024);

    std::vector<Member> ms = sample_members();
    int dtf = -1;
    if (!write_archive(dev, bf, ms, dtf)) { check(false, "append: multi-member data + manifest at EOD"); return finish(); }
    check(true, "append: multi-member data + manifest at EOD");
    std::printf("  (data archive at tape file %d)\n", dtf);

    Tape rtape(dev, bf);

    // Prerequisite: a sequential scan to learn each member's header position.
    rtape.scan_archive_with_blocks(
        dtf, bf,
        [&](const std::string &name, int64_t block, int64_t offset, const std::string &, uint64_t, time_t, mode_t) {
            for (auto &m : ms) if (m.name == name) { m.block = block; m.offset = offset; m.scanned = true; }
        },
        {});

    // The actual test: random-access read of each member via its (block, offset).
    for (auto &m : ms) {
        if (!m.scanned) { check(false, (std::string("position not found for ") + m.name).c_str()); continue; }
        Fd out; uint64_t sz = 0;
        bool ok = rtape.read_member(dtf, bf, m.block, m.offset, m.name, out, sz);
        std::vector<std::byte> got;
        if (ok && out.valid() && sz == m.content.size()) {
            got.resize(sz);
            ok = (::pread(out.get(), got.data(), sz, 0) == static_cast<ssize_t>(sz));
        } else ok = false;
        check(ok && got == m.content,
              (std::string("read_member seek+content: ") + m.name +
               " @block " + std::to_string(m.block) + "+" + std::to_string(m.offset)).c_str());
    }

    // Self-heal: feed read_member a deliberately WRONG offset that points INTO a
    // member's data instead of at its tar header. The fast read then hands libarchive
    // non-header bytes and fails; read_member must fall back to a name-scan from the
    // tape file's start, recover the content, and report the corrected (block, offset)
    // via its out-params — exactly what the FUSE layer writes back to fix a stale
    // position. We use the largest member (it certainly has a >=512-byte data record)
    // and offset = its header offset + 512, i.e. its first data record. Note: a wrong
    // offset that happens to land on ANOTHER member's header would be extracted
    // successfully (wrong member, no fallback), so it must point at data, not a header.
    const Member *heal = nullptr;
    for (auto &m : ms) if (m.scanned && (!heal || m.content.size() > heal->content.size())) heal = &m;
    const int64_t bsize = static_cast<int64_t>(bf) * 512;
    if (!heal || heal->offset + 512 >= bsize) {
        check(false, "no suitable member to exercise the offset-recovery fallback");
    } else {
        const int64_t wrong_offset = heal->offset + 512; // into the member's data, not its header
        Fd out; uint64_t sz = 0;
        int64_t found_block = -1, found_offset = -1;
        bool ok = rtape.read_member(dtf, bf, heal->block, wrong_offset, heal->name,
                                    out, sz, &found_block, &found_offset);
        std::vector<std::byte> got;
        if (ok && out.valid() && sz == heal->content.size()) {
            got.resize(sz);
            ok = (::pread(out.get(), got.data(), sz, 0) == static_cast<ssize_t>(sz));
        } else ok = false;
        check(ok && got == heal->content,
              (std::string("read_member recovers content after WRONG offset: ") + heal->name).c_str());
        check(found_block == heal->block && found_offset == heal->offset,
              (std::string("fallback reports corrected position: block ") +
               std::to_string(found_block) + "+" + std::to_string(found_offset) +
               " (expected " + std::to_string(heal->block) + "+" + std::to_string(heal->offset) + ")").c_str());
    }

    return finish();
}
