#!/bin/bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (C) 2026 Hugo Hurskainen
#
# test_cli_flags.sh — device-free test of the shared CLI argument parser
# (cli.cpp / parse_tape_opts), generalised from phase 5 ("flag hygiene") of the
# manual tapetest.sh. An unrecognised flag must be rejected with exit code 2 and
# an "unknown option" message BEFORE any tape is opened — so this needs no
# hardware and runs on every `make check`, guarding the parser the admin tools
# share. The device argument below is a placeholder that is never opened.

set -u
TT_NAME=cliflags
. "${TAPIR_TEST_SRCDIR:-.}/test_tape_lib.sh"
tt_require_binaries tfsck mktapir
tt_init

FAKE="/dev/tapir-no-such-device"   # parse fails before this is ever opened

say "=== test_cli_flags (device-free) ==="

# Run a command, expect exit code 2 and an "unknown option" diagnostic.
check_bad_flag(){ local name="$1"; shift
    local out; out="$("$@" 2>&1)"; local rc=$?
    if [ "$rc" -eq 2 ] && printf '%s' "$out" | grep -qi "unknown option"
    then ok "$name" "exit=$rc"
    else bad "$name" "exit=$rc out='${out%%$'\n'*}'"; fi; }

check_bad_flag "tfsck_verify_unknown_flag"   "$BIN/tfsck"   "$FAKE" --bogus-flag
check_bad_flag "mktapir_init_unknown_flag"   "$BIN/mktapir" "$FAKE" --bogus-flag
check_bad_flag "mktapir_import_unknown_flag" "$BIN/mktapir" import "$FAKE" --bogus-flag
check_bad_flag "mktapir_append_unknown_flag" "$BIN/mktapir" append "$FAKE" --bogus-flag

tt_finish
