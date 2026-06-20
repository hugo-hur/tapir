#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hugo Hurskainen
#
# test_tape_upgrade.sh — hardware test of `tfsck --upgrade-manifest` (phase 4 of
# the manual tapetest.sh). Simulates a pre-magic, old-format index: it takes a
# real tapir manifest and rewrites it as a plain manifest.json (no tapir PAX
# magic) at end of tape. The mount must then REFUSE (it cannot tell that index
# from an unrelated tar carrying a manifest.json), and `tfsck --upgrade-manifest`
# must rewrite it with the magic so the tape mounts again. Self-contained — first
# imports a one-member tar to create the index it then downgrades.
#
# SKIP (exit 77) unless TAPIR_TEST_DEVICE points at a scratch no-rewind tape and
# mt + tar are available. WARNING: writes from start of tape — SCRATCH only.

set -u
TT_NAME=upgrade
. "${TAPIR_TEST_SRCDIR:-.}/test_tape_lib.sh"
tt_require_device
tt_require_tools mt tar
tt_init

say "=== test_tape_upgrade on $DEV (manifest bf=$BF) ==="

# ── setup: one foreign tar imported into a real (magic) tapir index ───────────
add d_u upg_member 50000
mt -f "$DEV" setblk 0 >/dev/null 2>&1; mt -f "$DEV" rewind
( cd "$SRC/d_u" && tar -b 256 -H gnu -cf "$DEV" upg_member )   # f0
"$BIN/mktapir" import "$DEV" -m "$BF" >>"$WORK/setup_import.log" 2>&1
res $? "setup_import" "tapir index (with magic) at end of tape"

# ── extract that manifest and rewrite it PLAIN (no magic) at EOT ──────────────
mt -f "$DEV" eod; cnt=$(filenum); last=$(( cnt - 1 ))
mt -f "$DEV" rewind; mt -f "$DEV" fsf "$last"
if tar -b "$BF" -xOf "$DEV" manifest.json > "$SRC/manifest.json" 2>/dev/null && [ -s "$SRC/manifest.json" ]; then
    mt -f "$DEV" eod
    ( cd "$SRC" && tar -b "$BF" -H ustar -cf "$DEV" manifest.json )
    res $? "write_premagic_manifest" "plain manifest.json (no magic) at f$cnt"

    # ── mount must refuse the magic-less index ────────────────────────────────
    if expect_mount_fail "$WORK/mountfail.log"; then
        grep -qi "upgrade-manifest" "$WORK/mountfail.log" \
            && ok "mount_refuses_premagic" "told user to upgrade" \
            || ok "mount_refuses_premagic" "mount refused"
    else bad "mount_refuses_premagic" "mounted despite missing magic"; fi

    # ── upgrade re-adds the magic, restoring mountability ─────────────────────
    "$BIN/tfsck" --upgrade-manifest "$DEV" -m "$BF" >>"$WORK/upgrade.log" 2>&1
    res $? "upgrade_manifest"
    if mnt_up; then
        res "$(y "$(shaof "$MNT/upg_member")" "${SHA[upg_member]}")" "mount_after_upgrade" "member verified"
        mnt_down
    else bad "mount_after_upgrade" "still cannot mount"; fi
else
    bad "write_premagic_manifest" "could not extract manifest.json from f$last"
fi

tt_finish
