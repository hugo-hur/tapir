// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

// test_seek_plan.cpp — unit test for Tape::plan_seek (the tape-positioning
// decision logic), the part responsible for the import-positioning bug.
//
// The bug: seeking to file 0 from file N issued bsf(N+1), which crosses
// beginning-of-tape and errors. plan_seek is pure (no tape I/O), so we can both
// check specific cases and sweep all (current, target) pairs, asserting that no
// plan ever backward-spaces past BOT and that every plan lands on the target.
// Always runs (no hardware).

#include "tape.hpp"

#include <cstdio>

using namespace tapir;

static int g_pass = 0, g_fail = 0;
static inline void check(bool cond, const char *msg)
{
    if (cond) { ++g_pass; std::printf("  ok   %s\n", msg); }
    else { ++g_fail; std::fprintf(stderr, "  FAIL %s\n", msg); }
}

int main()
{
    using TS = Tape::TapeSeek;
    std::puts("=== test_seek_plan: Tape::plan_seek ===");

    // ── specific cases ──────────────────────────────────────────────────────────
    { TS p = Tape::plan_seek(-1, 5); check(p.rewind && p.bsf == 0 && p.fsf == 5, "unknown -> 5: rewind + fsf 5"); }
    { TS p = Tape::plan_seek(5, 5);  check(!p.rewind && p.bsf == 0 && p.fsf == 0, "5 -> 5: no-op"); }
    { TS p = Tape::plan_seek(2, 5);  check(!p.rewind && p.bsf == 0 && p.fsf == 3, "2 -> 5: fsf 3 (forward, no rewind)"); }
    { TS p = Tape::plan_seek(5, 2);  check(!p.rewind && p.bsf == 4 && p.fsf == 1, "5 -> 2: bsf 4 + fsf 1"); }

    // ── the regression: seeking to file 0 must REWIND, never bsf past BOT ────────
    { TS p = Tape::plan_seek(5, 0);  check(p.rewind && p.bsf == 0, "5 -> 0: rewind (not bsf 6 past BOT)"); }
    { TS p = Tape::plan_seek(1, 0);  check(p.rewind && p.bsf == 0, "1 -> 0: rewind"); }
    { TS p = Tape::plan_seek(-1, 0); check(p.rewind && p.bsf == 0, "unknown -> 0: rewind"); }

    // ── property sweep: lands on target AND never bsf's past beginning-of-tape ───
    int wrong_landing = 0, crosses_bot = 0;
    for (int cur = -1; cur <= 64; ++cur)
        for (int tgt = 0; tgt <= 64; ++tgt)
        {
            const TS p = Tape::plan_seek(cur, tgt);
            const int files_behind = cur < 0 ? 0 : cur; // filemarks available before `cur`
            if (p.rewind && p.bsf != 0)        ++crosses_bot; // never combine rewind with bsf
            if (!p.rewind && p.bsf > files_behind) ++crosses_bot; // bsf would pass BOT
            // simulate the resulting file position: [rewind->0] then -bsf then +fsf
            const int landed = (p.rewind ? 0 : cur) - p.bsf + p.fsf;
            if (landed != tgt) ++wrong_landing;
        }
    check(crosses_bot == 0,   "no plan backward-spaces past beginning-of-tape (bsf <= current)");
    check(wrong_landing == 0, "every plan lands exactly on the target file");

    std::printf("=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
