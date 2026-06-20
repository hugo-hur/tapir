#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hugo Hurskainen
#
# test_tape_mount.sh — hardware test of the FUSE mount + file I/O path.
#
# Exercises the parts of the stack the C++ tape tests do NOT: the `tapir` FUSE
# mount, writing real files through the kernel, read-after-write while staged,
# round-tripping across remounts, permission sealing, index-only delete, and
# tfsck verify / rollback. A trimmed, self-contained port of phase 1 of the
# manual tapetest.sh (it generates its own data — no external big file).
#
# Parameterisation matches the C++ hardware tests (see test_tape_common.hpp):
#   TAPIR_TEST_DEVICE        no-rewind scratch tape (…-nst). REQUIRED, else SKIP.
#   TAPIR_TEST_BLOCK_FACTOR  blocking factor ×512 (default 256 = 128 KiB).
#   TAPIR_TEST_BIGSIZE       bytes for the "large file" case (default 8 MiB).
#                            Set e.g. 5368709121 to exercise the >4 GiB off_t path.
#   TAPIR_BIN_DIR            where tapir/tfsck/mktapir live (default: build dir,
#                            exported by the Makefile's AM_TESTS_ENVIRONMENT).
#
#   make check                              -> SKIP (exit 77), no hardware touched
#   TAPIR_TEST_DEVICE=/dev/nst0 make check  -> runs against that scratch drive
#
# WARNING: --force reformats the tape; only point this at a SCRATCH cartridge.
# Exit: 0 all passed, 1 a check failed, 77 skipped (no device / no fusermount3).

set -u

DEV="${TAPIR_TEST_DEVICE:-}"
if [ -z "$DEV" ]; then
    echo "SKIP: set TAPIR_TEST_DEVICE=<scratch no-rewind tape> to run"
    exit 77
fi
if ! command -v fusermount3 >/dev/null 2>&1; then
    echo "SKIP: fusermount3 not found — cannot exercise the FUSE mount"
    exit 77
fi

BF="${TAPIR_TEST_BLOCK_FACTOR:-256}"
BIGSIZE="${TAPIR_TEST_BIGSIZE:-$((8 * 1024 * 1024))}"
BIN="${TAPIR_BIN_DIR:-.}"

for tool in tapir tfsck mktapir; do
    if [ ! -x "$BIN/$tool" ]; then
        echo "SKIP: $BIN/$tool not built — run from the build tree"
        exit 77
    fi
done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tapir-mount-test.XXXXXX")"
MNT="$WORK/mnt"; SRC="$WORK/src"
mkdir -p "$MNT" "$SRC"

pass=0; fail=0
TAPIR_PID=""
declare -A SHA

ts(){ date '+%H:%M:%S'; }
say(){ echo "[$(ts)] $*"; }
ok(){  pass=$((pass+1)); say "PASS  $1 ${2:+| $2}"; }
bad(){ fail=$((fail+1)); say "FAIL  $1 ${2:+| $2}"; }
res(){ if [ "$1" -eq 0 ]; then ok "$2" "${3:-}"; else bad "$2" "${3:-}"; fi; }
y(){ [ "$1" = "$2" ] && echo 0 || echo 1; }            # equality -> 0/1 for res()
shaof(){ sha256sum "$1" 2>/dev/null | awk '{print $1}'; }
mkf(){ head -c "$2" /dev/urandom > "$SRC/$1"; SHA[$1]=$(shaof "$SRC/$1"); }   # name size

# Background-mount tapir and wait for the mountpoint to appear (or the process
# to die). Returns non-zero if the mount never came up.
mnt_up(){ local i=0
    "$BIN/tapir" "$DEV" "$MNT" -b "$BF" -f >>"$WORK/tapir.log" 2>&1 &
    TAPIR_PID=$!
    while ! mountpoint -q "$MNT"; do
        sleep 1; i=$((i+1))
        kill -0 "$TAPIR_PID" 2>/dev/null || return 1
        [ $i -gt 120 ] && return 1
    done
    return 0; }
# Unmount and wait for tapir to flush its manifest and exit. The writer can take
# a while to drain to tape, so allow a generous timeout before force-killing.
mnt_down(){ local i=0
    fusermount3 -u "$MNT" 2>/dev/null
    while kill -0 "$TAPIR_PID" 2>/dev/null; do
        sleep 2; i=$((i+1))
        [ $i -gt 1200 ] && { kill -9 "$TAPIR_PID" 2>/dev/null; return 1; }
    done
    return 0; }

cleanup(){
    mountpoint -q "$MNT" && fusermount3 -u "$MNT" 2>/dev/null
    [ -n "$TAPIR_PID" ] && kill -0 "$TAPIR_PID" 2>/dev/null && kill -9 "$TAPIR_PID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

say "=== test_tape_mount on $DEV (block factor $BF = $((BF * 512 / 1024)) KiB) ==="
command -v mt >/dev/null 2>&1 && mt -f "$DEV" setblk 0 >/dev/null 2>&1   # variable block mode

# ── format + empty mount ──────────────────────────────────────────────────────
"$BIN/mktapir" "$DEV" --force -m "$BF" >>"$WORK/mktapir.log" 2>&1
res $? "init_force" "blank index at start of tape"

if mnt_up; then
    n=$(ls -A "$MNT" | wc -l); res "$(y "$n" 0)" "mount_empty" "entries=$n"
    mnt_down; res $? "unmount_empty"
else bad "mount_empty" "mount failed"; fi

# ── session 1: small files + read-after-write ─────────────────────────────────
mkf small_zero 0; mkf small_1b 1; mkf small_1k 1024
mkf small_64k 65536; mkf small_1m 1048576; mkf exec_file 4096
if mnt_up; then
    for f in small_zero small_1b small_1k small_64k small_1m; do cp "$SRC/$f" "$MNT/$f"; done
    install -m 0755 "$SRC/exec_file" "$MNT/exec_file"
    raw=0
    for f in small_1k small_1m exec_file; do
        [ "$(shaof "$MNT/$f")" = "${SHA[$f]}" ] || raw=1
    done
    res $raw "read_after_write" "staged read-back before flush"
    mnt_down; res $? "unmount_session1"
else bad "read_after_write" "mount failed"; fi

# ── remount round-trip + permission sealing ───────────────────────────────────
if mnt_up; then
    v=0
    for f in small_zero small_1b small_1k small_64k small_1m exec_file; do
        [ "$(shaof "$MNT/$f")" = "${SHA[$f]}" ] || { v=1; say "  mismatch $f"; }
    done
    res $v "roundtrip_small" "6 files verified after remount"
    res "$(y "$(stat -c '%A' "$MNT/small_1k")" "-r--r--r--")" "perm_sealed_normal" "mode=$(stat -c '%A' "$MNT/small_1k")"
    res "$(y "$(stat -c '%A' "$MNT/exec_file")" "-r-xr-xr-x")" "perm_sealed_exec"   "mode=$(stat -c '%A' "$MNT/exec_file")"
    if chmod 0777 "$MNT/small_1k" 2>/dev/null; then bad "chmod_sealed_eperm" "chmod succeeded"; else ok "chmod_sealed_eperm"; fi
    mnt_down; res $? "unmount_roundtrip"
else bad "roundtrip_small" "mount failed"; fi

# ── large file (default 8 MiB; set TAPIR_TEST_BIGSIZE huge for the off_t path) ─
mkf big_file "$BIGSIZE"
if mnt_up; then cp "$SRC/big_file" "$MNT/big_file"; mnt_down; res $? "write_bigfile" "$BIGSIZE bytes"
else bad "write_bigfile" "mount failed"; fi
if mnt_up; then
    sz=$(stat -c %s "$MNT/big_file" 2>/dev/null); res "$(y "$sz" "$BIGSIZE")" "bigfile_size" "stat=$sz"
    got=$(shaof "$MNT/big_file"); res "$(y "$got" "${SHA[big_file]}")" "bigfile_sha" "got=${got:0:16}"
    mnt_down; res $? "unmount_bigfile"
else bad "bigfile_verify" "mount failed"; fi

# ── append session (writes a new data tape file, extends the index) ───────────
mkf appended_a 2048; mkf appended_b 524288
if mnt_up; then
    cp "$SRC/appended_a" "$MNT/appended_a"; cp "$SRC/appended_b" "$MNT/appended_b"
    mnt_down; res $? "append_session"
else bad "append_session" "mount failed"; fi

# ── tfsck: list generations + full verify ─────────────────────────────────────
"$BIN/tfsck" --list-generations "$DEV" -m "$BF" >"$WORK/listgen.out" 2>>"$WORK/tfsck.log"
res $? "list_generations" "generations=$(grep -c 'generation' "$WORK/listgen.out")"

"$BIN/tfsck" "$DEV" -b "$BF" -m "$BF" >>"$WORK/tfsck_verify.log" 2>&1; rc=$?
res $rc "tfsck_verify" "exit=$rc"
grep -q "FAILED" "$WORK/tfsck_verify.log" && bad "tfsck_no_failures" "reported FAILED" || ok "tfsck_no_failures"

# ── index-only delete, then rollback brings the file back ─────────────────────
if mnt_up; then rm "$MNT/appended_a"; mnt_down; res $? "delete_unmount"
else bad "delete_unmount" "mount failed"; fi
if mnt_up; then
    [ -e "$MNT/appended_a" ] && bad "delete_effective" "still present" || ok "delete_effective"
    res "$(y "$(shaof "$MNT/small_1m")" "${SHA[small_1m]}")" "others_intact_after_delete"
    mnt_down
else bad "delete_effective" "mount failed"; fi

"$BIN/tfsck" --rollback "$DEV" -m "$BF" >>"$WORK/rollback.log" 2>&1
res $? "rollback_previous"
if mnt_up; then
    [ -e "$MNT/appended_a" ] && ok "rollback_restored" "deleted file returned" || bad "rollback_restored" "not restored"
    mnt_down
else bad "rollback_restored" "mount failed"; fi

say "=== done: $pass passed, $fail failed ==="
[ "$fail" -eq 0 ] || exit 1
exit 0
