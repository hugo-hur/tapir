#!/bin/sh
# autogen.sh — (re)generate the Autotools build system (configure, Makefile.in,
# config.h.in, aux scripts). Run after a fresh checkout or any change to
# configure.ac / Makefile.am, then build with ./configure && make.
#
# Requires autoconf, automake and pkg-config. The C++ standard-detection macros
# are vendored in m4/, so autoconf-archive is NOT required.

set -e

cd "$(dirname "$0")"

if ! command -v autoreconf >/dev/null 2>&1; then
    echo "autogen.sh: 'autoreconf' not found — install autoconf and automake." >&2
    exit 1
fi

# autoreconf reads local macros from m4/ and installs aux scripts into build-aux/
# (per AC_CONFIG_MACRO_DIR / AC_CONFIG_AUX_DIR); make sure both exist first.
mkdir -p build-aux m4

autoreconf --install --force

echo
echo "Bootstrap complete. Next:"
echo "    ./configure && make"
