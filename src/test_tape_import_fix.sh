#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hugo Hurskainen
#
# test_tape_import_fix.sh — hardware test of the "raw tars after an index" repair
# path (phase 3 of the manual tapetest.sh). Imports two base tars, then appends
# two more raw tars AFTER the manifest. Verifies that: a blind full-scan import
# REFUSES (it would resurrect index-deleted files / mis-order overwrites), the
# mount FAILS while unindexed data sits after the index, and `mktapir import -f`
# with an explicit tape-file list repairs the index so everything mounts and
# verifies. Self-contained — writes its own tars from BOT.
#
# SKIP (exit 77) unless TAPIR_TEST_DEVICE points at a scratch no-rewind tape and
# mt + tar are available. WARNING: writes from start of tape — SCRATCH only.

set -u
TT_NAME=importfix
. "${TAPIR_TEST_SRCDIR:-.}/test_tape_lib.sh"
tt_require_device
tt_require_tools mt tar
tt_init

say "=== test_tape_import_fix on $DEV (manifest bf=$BF) ==="

add d_p3a p3a_1 4096;   add d_p3a p3a_2 200000
add d_p3b p3b_1 9000;   add d_p3b p3b_2 88888
add d_xa  extraA_1 4096; add d_xa  extraA_2 200000
add d_xb  extraB_1 9000; add d_xb  extraB_2 88888

# ── base layout: two foreign tars, then a tapir index ─────────────────────────
mt -f "$DEV" setblk 0 >/dev/null 2>&1; mt -f "$DEV" rewind
( cd "$SRC/d_p3a" && tar -b 256 -H gnu   -cf "$DEV" p3a_1 p3a_2 )   # f0
( cd "$SRC/d_p3b" && tar -b 128 -H posix -cf "$DEV" p3b_1 p3b_2 )   # f1
"$BIN/mktapir" import "$DEV" -m "$BF" >>"$WORK/base_import.log" 2>&1
res $? "base_import" "manifest written at f2"

# ── append raw tars AFTER the index (f3, f4) ──────────────────────────────────
mt -f "$DEV" eod
( cd "$SRC/d_xa" && tar -b 256 -H gnu   -cf "$DEV" extraA_1 extraA_2 ); ra=$?   # f3
( cd "$SRC/d_xb" && tar -b 128 -H posix -cf "$DEV" extraB_1 extraB_2 ); rb=$?   # f4
res $(( ra | rb )) "append_raw_after_index" "2 raw tars at f3,f4 after the index"

# ── a blind full-scan import must REFUSE (an index already exists) ────────────
"$BIN/mktapir" import "$DEV" -m "$BF" >>"$WORK/refuse.log" 2>&1; rc=$?
{ [ $rc -ne 0 ] && grep -qi "tapir index" "$WORK/refuse.log"; } \
    && ok "fullscan_refused" "exit=$rc" || bad "fullscan_refused" "exit=$rc"

# ── mount must FAIL while unindexed data sits after the index ─────────────────
expect_mount_fail "$WORK/mountfail.log" \
    && ok "mount_fails_after_raw_append" || bad "mount_fails_after_raw_append" "mounted unexpectedly"

# ── targeted import -f repairs the index (skip manifest at f2) ─────────────────
"$BIN/mktapir" import "$DEV" -f 0,1,3,4 -m "$BF" >>"$WORK/fix.log" 2>&1
res $? "import_fix" "rebuilt index incl. appended tars"
if mnt_up; then
    v=0
    for f in p3a_1 p3a_2 p3b_1 p3b_2 extraA_1 extraA_2 extraB_1 extraB_2; do
        [ "$(shaof "$MNT/$f")" = "${SHA[$f]}" ] || { v=1; say "  fix mismatch $f"; }
    done
    res $v "fix_verify" "base + appended tars all present"
    mnt_down
else bad "fix_verify" "mount failed"; fi

"$BIN/tfsck" "$DEV" -m "$BF" >>"$WORK/fixverify.log" 2>&1
res $? "tfsck_after_fix"

tt_finish
