#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hugo Hurskainen
#
# test_tape_import.sh — hardware test of `mktapir import` (phase 2 of the manual
# tapetest.sh). Lays down five foreign tars in five different formats at five
# different blocking factors, full-scan imports them into a fresh tapir index
# (per-file block size auto-detected), then mounts and verifies every member and
# runs tfsck. Proves the importer copes with V7/ustar/GNU/oldgnu/pax and mixed
# blocking factors in one pass. Self-contained — writes its own tars from BOT.
#
# SKIP (exit 77) unless TAPIR_TEST_DEVICE points at a scratch no-rewind tape and
# mt + tar are available. WARNING: writes from start of tape — SCRATCH only.

set -u
TT_NAME=import
. "${TAPIR_TEST_SRCDIR:-.}/test_tape_lib.sh"
tt_require_device
tt_require_tools mt tar
tt_init

# A name well past the ustar 100-char limit, to force the GNU longname extension.
GNULONG="gnu_long_name_well_past_the_ustar_one_hundred_character_limit_to_force_the_gnu_longname_extension_aaaaaaaaaaaaaaaaaa"

say "=== test_tape_import on $DEV (manifest bf=$BF) ==="

# ── five foreign tars, five formats, five blocking factors ────────────────────
# Distinct per-file mtimes (whole seconds: every tar format stores integer mtime)
# so the import → index → FUSE getattr path can be checked for preservation.
addt d_v7     v7_a    1000     1600000001; addt d_v7     v7_b    40000  1600000002
addt d_ustar  ustar_a 2000     1600000003; addt d_ustar  ustar_b 131072 1600000004
addt d_gnu    "$GNULONG" 70000 1600000005; addt d_gnu    gnu_b   300000 1600000006
addt d_oldgnu oldgnu_a 5000    1600000007; addt d_oldgnu oldgnu_b 250000 1600000008
addt d_pax    pax_a   123      1600000009; addt d_pax    pax_b   777777 1600000010
mt -f "$DEV" setblk 0 >/dev/null 2>&1; mt -f "$DEV" rewind
( cd "$SRC/d_v7"     && tar -b 64  -H v7     -cf "$DEV" v7_a v7_b );          r0=$?   # f0 bf=64
( cd "$SRC/d_ustar"  && tar -b 128 -H ustar  -cf "$DEV" ustar_a ustar_b );   r1=$?   # f1 bf=128
( cd "$SRC/d_gnu"    && tar -b 256 -H gnu    -cf "$DEV" "$GNULONG" gnu_b );   r2=$?   # f2 bf=256
( cd "$SRC/d_oldgnu" && tar -b 512 -H oldgnu -cf "$DEV" oldgnu_a oldgnu_b );  r3=$?   # f3 bf=512
( cd "$SRC/d_pax"    && tar -b 20  -H posix  -cf "$DEV" pax_a pax_b );        r4=$?   # f4 bf=20
res $(( r0|r1|r2|r3|r4 )) "write_foreign_tars" "v7/ustar/gnu/oldgnu/pax @ bf 64/128/256/512/20"

# ── full-scan import (auto-detects each file's block factor) ───────────────────
"$BIN/mktapir" import "$DEV" -m "$BF" -v >>"$WORK/import.log" 2>&1
res $? "import_fullscan"
if grep -q '(factor 64)' "$WORK/import.log" && grep -q '(factor 512)' "$WORK/import.log" \
   && grep -q '(factor 20)' "$WORK/import.log"
then ok "mixed_bf_detected" "auto-detected factors 64,512,20"
else bad "mixed_bf_detected" "mixed factors not in import log"; fi

# ── mount + verify every member's content AND mtime, then tfsck ───────────────
if mnt_up; then
    v=0; m=0
    for f in v7_a v7_b ustar_a ustar_b "$GNULONG" gnu_b oldgnu_a oldgnu_b pax_a pax_b; do
        [ "$(shaof "$MNT/$f")" = "${SHA[$f]}" ] || { v=1; say "  content mismatch $f"; }
        got=$(stat -c %Y "$MNT/$f" 2>/dev/null)
        [ "$got" = "${MT[$f]}" ] || { m=1; say "  mtime mismatch $f got=$got want=${MT[$f]}"; }
    done
    res $v "import_verify_allformats" "10 members / 5 formats"
    res $m "import_mtime_preserved" "mtime across v7/ustar/gnu/oldgnu/pax"
    mnt_down
else bad "import_verify_allformats" "mount failed"; fi

"$BIN/tfsck" "$DEV" -m "$BF" >>"$WORK/import_verify.log" 2>&1
res $? "tfsck_import"

tt_finish
