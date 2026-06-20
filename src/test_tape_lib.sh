# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hugo Hurskainen
#
# test_tape_lib.sh — shared helpers for the hardware tape tests. SOURCED, not run
# (it is not listed in TESTS). Provides environment resolution, the SKIP gates,
# pass/fail tally, file/sha helpers, and FUSE mount/unmount plumbing used by
# test_tape_mount/import/import_fix/upgrade. Test data is generated on the fly, so
# no external fixtures are needed.
#
# Runtime parameters (shared with the C++ tape tests, see test_tape_common.hpp):
#   TAPIR_TEST_DEVICE        no-rewind scratch tape (…-nst). REQUIRED for the
#                            mount/import tests, else they SKIP (exit 77).
#   TAPIR_TEST_BLOCK_FACTOR  blocking factor ×512 (default 256 = 128 KiB).
#   TAPIR_TEST_BIGSIZE       bytes for the "large file" case (default 8 MiB).
#   TAPIR_BIN_DIR            where tapir/tfsck/mktapir live (default: build dir).
#   TAPIR_TEST_SRCDIR        where this lib + the scripts live (default: cwd).
#                            Both are exported by the Makefile AM_TESTS_ENVIRONMENT.
#
# WARNING: the device tests reformat the tape (mktapir --force / writing from
# BOT) — only ever point TAPIR_TEST_DEVICE at a SCRATCH cartridge. Run serially;
# do not parallelise hardware tests against a single drive.

# ── pass/fail tally ───────────────────────────────────────────────────────────
pass=0; fail=0
ts(){ date '+%H:%M:%S'; }
say(){ echo "[$(ts)] $*"; }
ok(){  pass=$((pass+1)); say "PASS  $1 ${2:+| $2}"; }
bad(){ fail=$((fail+1)); say "FAIL  $1 ${2:+| $2}"; }
res(){ if [ "$1" -eq 0 ]; then ok "$2" "${3:-}"; else bad "$2" "${3:-}"; fi; }
y(){ [ "$1" = "$2" ] && echo 0 || echo 1; }                 # equality -> 0/1 for res()
tt_finish(){ say "=== done: $pass passed, $fail failed ==="; [ "$fail" -eq 0 ] || exit 1; exit 0; }

# ── environment + SKIP gates ──────────────────────────────────────────────────
DEV="${TAPIR_TEST_DEVICE:-}"
BF="${TAPIR_TEST_BLOCK_FACTOR:-256}"
BIGSIZE="${TAPIR_TEST_BIGSIZE:-$((8 * 1024 * 1024))}"
BIN="${TAPIR_BIN_DIR:-.}"

tt_skip(){ echo "SKIP: $*"; exit 77; }
tt_require_binaries(){ local t
  for t in "$@"; do [ -x "$BIN/$t" ] || tt_skip "$BIN/$t not built — run from the build tree"; done; }
tt_require_tools(){ local t
  for t in "$@"; do command -v "$t" >/dev/null 2>&1 || tt_skip "$t not found — required for this test"; done; }
tt_require_device(){
  [ -n "$DEV" ] || tt_skip "set TAPIR_TEST_DEVICE=<scratch no-rewind tape> to run"
  command -v fusermount3 >/dev/null 2>&1 || tt_skip "fusermount3 not found — cannot exercise the FUSE mount"
  tt_require_binaries tapir tfsck mktapir; }

# ── working dirs + test data ──────────────────────────────────────────────────
declare -A SHA
TAPIR_PID=""
tt_init(){
  WORK="$(mktemp -d "${TMPDIR:-/tmp}/tapir-${TT_NAME:-test}.XXXXXX")"
  MNT="$WORK/mnt"; SRC="$WORK/src"; mkdir -p "$MNT" "$SRC"
  trap tt_cleanup EXIT
  [ -n "$DEV" ] && command -v mt >/dev/null 2>&1 && mt -f "$DEV" setblk 0 >/dev/null 2>&1   # variable block
}
tt_cleanup(){
  [ -n "${MNT:-}" ] && mountpoint -q "$MNT" 2>/dev/null && fusermount3 -u "$MNT" 2>/dev/null
  [ -n "$TAPIR_PID" ] && kill -0 "$TAPIR_PID" 2>/dev/null && kill -9 "$TAPIR_PID" 2>/dev/null
  [ -n "${WORK:-}" ] && rm -rf "$WORK"
}
declare -A MT   # expected mtime (epoch seconds) per member, for timestamp checks
shaof(){ sha256sum "$1" 2>/dev/null | awk '{print $1}'; }
mkf(){ head -c "$2" /dev/urandom > "$SRC/$1"; SHA[$1]=$(shaof "$SRC/$1"); }              # name size
add(){ mkdir -p "$SRC/$1"; head -c "$3" /dev/urandom > "$SRC/$1/$2"; SHA[$2]=$(shaof "$SRC/$1/$2"); }  # dir name size
addt(){ add "$1" "$2" "$3"; touch -d "@$4" "$SRC/$1/$2"; MT[$2]="$4"; }                  # dir name size mtime-epoch
filenum(){ mt -f "$DEV" status | sed -n 's/.*File number=\([0-9]*\).*/\1/p'; }

# ── FUSE mount plumbing ───────────────────────────────────────────────────────
# Background-mount tapir and wait for the mountpoint (or the process to die).
mnt_up(){ local bf="${1:-$BF}" i=0
  "$BIN/tapir" "$DEV" "$MNT" -b "$bf" -f >>"$WORK/tapir.log" 2>&1 & TAPIR_PID=$!
  while ! mountpoint -q "$MNT"; do sleep 1; i=$((i+1))
    kill -0 "$TAPIR_PID" 2>/dev/null || return 1; [ $i -gt 120 ] && return 1; done
  return 0; }
# Unmount and wait for tapir to flush its manifest and exit (writer drain can be slow).
mnt_down(){ local i=0; fusermount3 -u "$MNT" 2>/dev/null
  while kill -0 "$TAPIR_PID" 2>/dev/null; do sleep 2; i=$((i+1))
    [ $i -gt 1200 ] && { kill -9 "$TAPIR_PID" 2>/dev/null; return 1; }; done
  return 0; }
# Launch tapir expecting it to FAIL to mount; return 0 if no mountpoint appears.
expect_mount_fail(){ local log="$1" mpid i=0
  "$BIN/tapir" "$DEV" "$MNT" -b "$BF" -f >>"$log" 2>&1 & mpid=$!
  while ! mountpoint -q "$MNT"; do sleep 1; i=$((i+1)); kill -0 $mpid 2>/dev/null || break; [ $i -gt 30 ] && break; done
  if mountpoint -q "$MNT"; then fusermount3 -u "$MNT" 2>/dev/null; wait $mpid 2>/dev/null; return 1; fi
  wait $mpid 2>/dev/null; return 0; }
