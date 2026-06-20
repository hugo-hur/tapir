#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hugo Hurskainen
#
# test_tape_append.sh — hardware test of `mktapir append <file>` re-streaming a
# tar FILE from disk onto tape, including COMPRESSED inputs. libarchive's filter
# auto-detection means a .tar, .tar.gz, or .tar.xz on disk all re-stream to tape
# as a plain (uncompressed) tar; their members are then read back through the FUSE
# mount and verified. Also checks per-member timestamp preservation across the
# from-file import path. (We assume tapes themselves never hold compressed data
# archives — only on-disk input files may be compressed.)
#
# SKIP (exit 77) unless TAPIR_TEST_DEVICE points at a scratch no-rewind tape and
# tar + gzip are available; the .tar.xz case is skipped if xz is missing.
# WARNING: --force reformats the tape; SCRATCH cartridges only.

set -u
TT_NAME=append
. "${TAPIR_TEST_SRCDIR:-.}/test_tape_lib.sh"
tt_require_device
tt_require_tools tar gzip
tt_init

say "=== test_tape_append on $DEV (bf=$BF) ==="

# Source members with distinct whole-second mtimes (preserved by every tar format).
addt d_plain plain_a 4096   1600000101; addt d_plain plain_b 200000 1600000102
addt d_gz    gz_a    8000   1600000201; addt d_gz    gz_b    150000 1600000202
addt d_xz    xz_a    12000  1600000301; addt d_xz    xz_b    90000  1600000302

# Pack the members into a plain tar, a gzip tar, and (if xz is present) an xz tar.
( cd "$SRC/d_plain" && tar -cf  "$WORK/plain.tar" plain_a plain_b ); res $? "pack_plain_tar"
( cd "$SRC/d_gz"    && tar -czf "$WORK/gz.tar.gz" gz_a gz_b );       res $? "pack_tar_gz"
have_xz=0
if command -v xz >/dev/null 2>&1; then
    ( cd "$SRC/d_xz" && tar -cJf "$WORK/xz.tar.xz" xz_a xz_b ) && have_xz=1
    res $((1 - have_xz)) "pack_tar_xz"
else
    say "SKIP-sub: xz not found — skipping the tar.xz case"
fi

# Fresh tapir tape, then append each archive (filter auto-detected from content).
"$BIN/mktapir" "$DEV" --force -m "$BF" >>"$WORK/mktapir.log" 2>&1; res $? "init_force"
"$BIN/mktapir" append "$DEV" "$WORK/plain.tar" -b "$BF" -m "$BF" >>"$WORK/append_plain.log" 2>&1
res $? "append_plain_tar"
"$BIN/mktapir" append "$DEV" "$WORK/gz.tar.gz" -b "$BF" -m "$BF" >>"$WORK/append_gz.log" 2>&1
res $? "append_tar_gz"
if [ "$have_xz" -eq 1 ]; then
    "$BIN/mktapir" append "$DEV" "$WORK/xz.tar.xz" -b "$BF" -m "$BF" >>"$WORK/append_xz.log" 2>&1
    res $? "append_tar_xz"
fi

# Mount once and verify content + mtime for every appended member.
members="plain_a plain_b gz_a gz_b"
[ "$have_xz" -eq 1 ] && members="$members xz_a xz_b"
if mnt_up; then
    v=0; m=0
    for f in $members; do
        [ "$(shaof "$MNT/$f")" = "${SHA[$f]}" ] || { v=1; say "  content mismatch $f"; }
        got=$(stat -c %Y "$MNT/$f" 2>/dev/null)
        [ "$got" = "${MT[$f]}" ] || { m=1; say "  mtime mismatch $f got=$got want=${MT[$f]}"; }
    done
    res $v "append_verify_content" "members re-streamed from disk archives"
    res $m "append_mtime_preserved" "mtime preserved across all appended archives"
    mnt_down
else bad "append_verify_content" "mount failed"; fi

"$BIN/tfsck" "$DEV" -b "$BF" -m "$BF" >>"$WORK/tfsck.log" 2>&1
res $? "tfsck_append"

tt_finish
