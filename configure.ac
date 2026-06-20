#                                               -*- Autoconf -*-
# configure.ac — tapir build configuration
#
# Verifies the project's requirements:
#   1. C++20 compiler (hard minimum); C++23 preferred for the features below.
#   2. std::expected and std::print (C++23) — used if available.
#   3. a usable libarchive >= 3.0.0 with PAX support.
#   4. libfuse 3 (>= 3.0), required.
#   5. nlohmann/json (>= 3.0) for parsing the manifest index.
#   6. libcrypto (OpenSSL) for SHA-256 of appended files.
#
# Bootstrap with:  autoreconf -i   (the C++ std macros are vendored in m4/, so
# autoconf-archive is NOT required; pkg-config is used for the libraries).

AC_PREREQ([2.69])
AC_INIT([tapir], [0.1.0], [hugo.hurskainen@gmail.com])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR([src/tapir.cpp])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([foreign subdir-objects -Wall])
AM_PROG_AR

# ── 1. C++ compiler + standard ────────────────────────────────────────────────
# C++20 is the hard minimum. C++23 is preferred (for std::expected / std::print)
# and selected automatically when available; control with --enable-cxx23.
AC_PROG_CXX
AC_PROG_RANLIB

AC_ARG_ENABLE([cxx23],
  [AS_HELP_STRING([--enable-cxx23@<:@=auto|yes|no@:>@],
     [build in C++23 mode for std::expected/std::print @<:@default=auto@:>@])],
  [], [enable_cxx23=auto])

ax_std=cxx20
AS_IF([test "x$enable_cxx23" != "xno"],
  [AX_CXX_TRY_STDCXX_23([ax_std=cxx23], [ax_std=cxx20])])

AS_IF([test "x$ax_std" != "xcxx23"], [
  AS_IF([test "x$enable_cxx23" = "xyes"],
    [AC_MSG_ERROR([--enable-cxx23=yes but the compiler has no working C++23 mode])])
  AX_CXX_COMPILE_STDCXX_20
])
AC_DEFINE([HAVE_CXX20], [1], [Define since the project requires at least C++20.])

# ── 2. C++23 library features we use (only meaningful in C++23 mode) ───────────
have_std_expected=no
have_std_print=no
AS_IF([test "x$ax_std" = "xcxx23"], [
  AC_LANG_PUSH([C++])
  AC_CACHE_CHECK([for std::expected (<expected>)], [ax_cv_std_expected],
    [AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <expected>]],
       [[std::expected<int,int> e{42};
         std::expected<int,int> u{std::unexpected<int>(7)};
         return (e.value() == 42 && !u) ? 0 : 1;]])],
       [ax_cv_std_expected=yes], [ax_cv_std_expected=no])])
  have_std_expected=$ax_cv_std_expected

  AC_CACHE_CHECK([for std::print (<print>)], [ax_cv_std_print],
    [AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <print>]],
       [[std::println("the answer is {}", 42);]])],
       [ax_cv_std_print=yes], [ax_cv_std_print=no])])
  have_std_print=$ax_cv_std_print
  AC_LANG_POP([C++])
])

# If C++23 was explicitly requested, missing features are fatal.
AS_IF([test "x$enable_cxx23" = "xyes"], [
  AS_IF([test "x$have_std_expected" != "xyes"],
    [AC_MSG_ERROR([--enable-cxx23=yes but std::expected (<expected>) is unavailable])])
  AS_IF([test "x$have_std_print" != "xyes"],
    [AC_MSG_ERROR([--enable-cxx23=yes but std::print (<print>) is unavailable])])
])

AS_IF([test "x$have_std_expected" = "xyes"],
  [AC_DEFINE([HAVE_STD_EXPECTED], [1], [Define if <expected>/std::expected is usable.])])
AS_IF([test "x$have_std_print" = "xyes"],
  [AC_DEFINE([HAVE_STD_PRINT], [1], [Define if <print>/std::print is usable.])])

# ── 3. usable libarchive ──────────────────────────────────────────────────────
# archive_write_header_position() lets mktapir append record exact tape_block /
# tape_block_offset in one write pass without a second tape read.  It is not in
# upstream libarchive; it lives in the tapir fork (deps/libarchive submodule,
# branch HeaderPositionExtract).
#
# Detection strategy (default / auto):
#   1. Locate system libarchive via pkg-config or default search paths.
#   2. Link-test for archive_write_header_position().
#   3a. Feature present → use system as-is.
#   3b. Feature absent + deps/libarchive initialised → auto-build the fork.
#   3c. Feature absent + submodule missing → warn, continue without offsets.
#
# Overrides:
#   --with-libarchive=bundled   Always build deps/libarchive (useful to force
#                               the fork even if the system has the feature).
#   --with-libarchive=system    Use system only; error if feature is absent.
#   --with-libarchive=PATH      Use the installation at PATH (include/lib[64]).
#
AC_ARG_WITH([libarchive],
  [AS_HELP_STRING([--with-libarchive=@<:@bundled|system|PATH@:>@],
     [force bundled fork, use only system libarchive (error if feature absent),
      or use the installation at PATH; default: auto — system if
      archive_write_header_position is present, bundled fork otherwise])],
  [with_libarchive=$withval],
  [with_libarchive=auto])

PKG_PROG_PKG_CONFIG

dnl Absolute path to the submodule source tree (used for auto-fallback check too).
tapir_la_src="$(cd "${srcdir}" && pwd)/deps/libarchive"
tapir_use_bundled=no

dnl ── Step 1: resolve LIBARCHIVE_CFLAGS / LIBARCHIVE_LIBS ───────────────────
AS_IF([test "x$with_libarchive" = "xbundled"], [
  dnl Force bundled; skip system probe entirely.
  tapir_use_bundled=yes

], [test "x$with_libarchive" != "xauto" && test "x$with_libarchive" != "xsystem"], [
  dnl Explicit PATH: set flags directly, no system probe, no auto-fallback.
  la_libdir="${with_libarchive}/lib"
  AS_IF([test -d "${with_libarchive}/lib64"], [la_libdir="${with_libarchive}/lib64"])
  LIBARCHIVE_CFLAGS="-I${with_libarchive}/include"
  AS_IF([test -n "$PKG_CONFIG" && test -f "${la_libdir}/pkgconfig/libarchive.pc"], [
    la_old_pkgpath="$PKG_CONFIG_PATH"
    PKG_CONFIG_PATH="${la_libdir}/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
    export PKG_CONFIG_PATH
    LIBARCHIVE_LIBS=$("$PKG_CONFIG" --libs --static libarchive 2>/dev/null)
    PKG_CONFIG_PATH="$la_old_pkgpath"
    export PKG_CONFIG_PATH
  ])
  AS_IF([test -z "$LIBARCHIVE_LIBS"], [
    LIBARCHIVE_LIBS="-L${la_libdir} -larchive -Wl,-rpath,${la_libdir}"
  ])
  have_libarchive=yes
  AC_MSG_NOTICE([libarchive: using installation at ${with_libarchive}])

], [
  dnl Auto or system: locate via pkg-config, then default paths.
  PKG_CHECK_MODULES([LIBARCHIVE], [libarchive >= 3.0.0],
                    [have_libarchive=yes],
                    [have_libarchive=no])
  AS_IF([test "x$have_libarchive" != "xyes"], [
    AC_MSG_NOTICE([pkg-config did not find libarchive; trying default paths])
    AC_LANG_PUSH([C++])
    AC_CHECK_HEADER([archive.h], [],
      [AC_MSG_ERROR([archive.h not found — install libarchive-dev or use --with-libarchive=bundled])])
    AC_CHECK_HEADER([archive_entry.h], [],
      [AC_MSG_ERROR([archive_entry.h not found — install libarchive-dev or use --with-libarchive=bundled])])
    AC_SEARCH_LIBS([archive_read_new], [archive],
      [LIBARCHIVE_LIBS="-larchive"],
      [AC_MSG_ERROR([libarchive not found — install libarchive-dev or use --with-libarchive=bundled])])
    AC_LANG_POP([C++])
    LIBARCHIVE_CFLAGS=""
    have_libarchive=yes
  ])

  dnl ── Step 2: probe system libarchive for archive_write_header_position ───
  ax_save_CPPFLAGS="$CPPFLAGS"; CPPFLAGS="$CPPFLAGS $LIBARCHIVE_CFLAGS"
  ax_save_LIBS="$LIBS";         LIBS="$LIBARCHIVE_LIBS $LIBS"
  AC_LANG_PUSH([C++])
  AC_MSG_CHECKING([whether system libarchive has archive_write_header_position])
  AC_LINK_IFELSE(
    [AC_LANG_PROGRAM([[#include <archive.h>]],
       [[struct archive *a = archive_write_new();
         la_int64_t p = archive_write_header_position(a); (void)p;
         return archive_write_free(a);]])],
    [AC_MSG_RESULT([yes])],
    [AC_MSG_RESULT([no — checking fallback])
     dnl Feature absent: decide based on --with-libarchive value.
     AS_IF([test "x$with_libarchive" = "xsystem"], [
       AC_MSG_ERROR([--with-libarchive=system: system libarchive lacks
archive_write_header_position.  Drop --with-libarchive=system to let configure
auto-build the bundled fork, or run:  git submodule update --init deps/libarchive])
     ], [test -f "${tapir_la_src}/configure.ac"], [
       AC_MSG_NOTICE([auto-building bundled libarchive fork (deps/libarchive)])
       tapir_use_bundled=yes
     ], [
       AC_MSG_WARN([system libarchive lacks archive_write_header_position and the
deps/libarchive submodule is not initialised.  mktapir append will record
block=-1 for all members (tfsck can fill them in later).
To enable single-pass block offsets:
  git submodule update --init deps/libarchive && ./configure])
     ])
    ])
  AC_LANG_POP([C++])
  CPPFLAGS="$ax_save_CPPFLAGS"; LIBS="$ax_save_LIBS"
])

dnl ── Step 3: build the bundled fork if flagged ──────────────────────────────
dnl Shared by --with-libarchive=bundled and the auto-fallback path above.
AS_IF([test "x$tapir_use_bundled" = "xyes"], [
  AS_IF([! test -f "${tapir_la_src}/configure.ac"], [
    AC_MSG_ERROR([deps/libarchive submodule is not initialised.
Run:  git submodule update --init deps/libarchive])])
  AS_IF([! test -f "${tapir_la_src}/configure"], [
    AC_MSG_NOTICE([Bootstrapping autotools in deps/libarchive (build/autogen.sh) ...])
    (cd "${tapir_la_src}" && sh build/autogen.sh >/dev/null 2>&1)
    AS_IF([! test -f "${tapir_la_src}/configure"], [
      AC_MSG_ERROR([autogen.sh did not produce a configure script in deps/libarchive])])
  ])
  la_build="${ac_pwd}/bundled-libarchive-build"
  la_inst="${ac_pwd}/bundled-libarchive"
  la_log="${ac_pwd}/bundled-libarchive.log"
  AS_MKDIR_P(["${la_build}"])
  AC_MSG_NOTICE([Configuring bundled libarchive (log: bundled-libarchive.log) ...])
  (cd "${la_build}" && "${tapir_la_src}/configure" \
      --prefix="${la_inst}" \
      --disable-shared --enable-static \
      --with-pic \
      --without-openssl --without-xml2 --without-expat \
      --without-nettle --without-mbedtls \
      --disable-bsdtar --disable-bsdcpio \
      --disable-dependency-tracking) >"${la_log}" 2>&1
  AS_IF([test $? -ne 0], [
    AC_MSG_ERROR([bundled libarchive configure failed — see ${la_log}])])
  AC_MSG_NOTICE([Building bundled libarchive ...])
  (cd "${la_build}" && ${MAKE:-make} install) >>"${la_log}" 2>&1
  AS_IF([test $? -ne 0], [
    AC_MSG_ERROR([bundled libarchive build failed — see ${la_log}])])
  AC_MSG_NOTICE([Bundled libarchive installed (static) at ${la_inst}])
  dnl pkg-config --static gives the full link line (zlib, bz2, lzma, etc.).
  la_libdir="${la_inst}/lib"
  AS_IF([test -d "${la_inst}/lib64"], [la_libdir="${la_inst}/lib64"])
  LIBARCHIVE_CFLAGS="-I${la_inst}/include"
  AS_IF([test -n "$PKG_CONFIG" && test -f "${la_libdir}/pkgconfig/libarchive.pc"], [
    la_old_pkgpath="$PKG_CONFIG_PATH"
    PKG_CONFIG_PATH="${la_libdir}/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
    export PKG_CONFIG_PATH
    LIBARCHIVE_LIBS=$("$PKG_CONFIG" --libs --static libarchive 2>/dev/null)
    PKG_CONFIG_PATH="$la_old_pkgpath"
    export PKG_CONFIG_PATH
  ])
  AS_IF([test -z "$LIBARCHIVE_LIBS"], [
    LIBARCHIVE_LIBS="-L${la_libdir} -larchive -lz -lbz2 -llzma"
  ])
  have_libarchive=yes
  with_libarchive="${la_inst}"
])

dnl ── Functional gate: PAX writer usable + archive_write_header_position ─────
ax_save_CPPFLAGS="$CPPFLAGS"
ax_save_LIBS="$LIBS"
CPPFLAGS="$CPPFLAGS $LIBARCHIVE_CFLAGS"
LIBS="$LIBARCHIVE_LIBS $LIBS"
AC_LANG_PUSH([C++])
AC_MSG_CHECKING([whether libarchive is usable (>= 3.0.0, PAX writer)])
AC_LINK_IFELSE(
  [AC_LANG_PROGRAM(
     [[#include <archive.h>
       #include <archive_entry.h>
       #if ARCHIVE_VERSION_NUMBER < 3000000
       #  error "libarchive >= 3.0.0 is required"
       #endif
     ]],
     [[struct archive *a = archive_write_new();
       archive_write_set_format_pax_restricted(a);
       struct archive_entry *e = archive_entry_new();
       archive_entry_free(e);
       return archive_write_free(a);]])],
  [AC_MSG_RESULT([yes])],
  [AC_MSG_RESULT([no])
   AC_MSG_ERROR([a usable libarchive >= 3.0.0 with PAX support was not found])])
AC_MSG_CHECKING([for archive_write_header_position])
AC_LINK_IFELSE(
  [AC_LANG_PROGRAM(
     [[#include <archive.h>]],
     [[struct archive *a = archive_write_new();
       la_int64_t p = archive_write_header_position(a);
       (void)p;
       return archive_write_free(a);]])],
  [AC_MSG_RESULT([yes])
   AC_DEFINE([HAVE_ARCHIVE_WRITE_HEADER_POSITION], [1],
     [Define if archive_write_header_position() is available (tapir libarchive fork).])],
  [AC_MSG_RESULT([no — append will record block=-1])])
AC_LANG_POP([C++])
CPPFLAGS="$ax_save_CPPFLAGS"
LIBS="$ax_save_LIBS"

# ── 4. libfuse 3 (required) ───────────────────────────────────────────────────
PKG_CHECK_MODULES([FUSE], [fuse3 >= 3.0], [],
  [AC_MSG_ERROR([libfuse3 (>= 3.0) is required — install libfuse3-dev and fuse3])])
AC_DEFINE([HAVE_FUSE3], [1], [Define since the project requires libfuse 3.x.])
AC_DEFINE([FUSE_USE_VERSION], [31], [FUSE API version to compile against.])

# ── 5. nlohmann/json (manifest parsing) ───────────────────────────────────────
PKG_CHECK_MODULES([JSON], [nlohmann_json >= 3.0], [],
  [AC_MSG_NOTICE([pkg-config has no nlohmann_json; trying the default include path])
   AC_LANG_PUSH([C++])
   AC_CHECK_HEADER([nlohmann/json.hpp], [],
     [AC_MSG_ERROR([nlohmann/json.hpp not found — install nlohmann-json3-dev])])
   AC_LANG_POP([C++])
   JSON_CFLAGS=""; JSON_LIBS=""])

# Functional gate: confirm the header actually compiles under our C++ standard.
ax_save_CPPFLAGS="$CPPFLAGS"
CPPFLAGS="$CPPFLAGS $JSON_CFLAGS"
AC_LANG_PUSH([C++])
AC_MSG_CHECKING([whether nlohmann/json is usable])
AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM([[#include <nlohmann/json.hpp>]],
     [[auto j = nlohmann::json::parse("[1,2,3]"); return static_cast<int>(j.size()) - 3;]])],
  [AC_MSG_RESULT([yes])],
  [AC_MSG_RESULT([no])
   AC_MSG_ERROR([nlohmann/json header found but does not compile])])
AC_LANG_POP([C++])
CPPFLAGS="$ax_save_CPPFLAGS"

# ── 6. libcrypto (SHA-256 for appended files) ─────────────────────────────────
PKG_CHECK_MODULES([CRYPTO], [libcrypto >= 1.1], [],
  [AC_MSG_ERROR([libcrypto (OpenSSL) >= 1.1 is required — install libssl-dev])])

AC_CONFIG_FILES([Makefile src/Makefile])
AC_OUTPUT

AS_IF([test "x$tapir_use_bundled" = "xyes"],
  [tapir_la_info="bundled fork (${with_libarchive})"],
  [AS_IF([test "x$with_libarchive" = "xauto" || test "x$with_libarchive" = "xsystem"],
     [tapir_la_info="system"],
     [tapir_la_info="${with_libarchive}"])])

AC_MSG_NOTICE([

  $PACKAGE_NAME $PACKAGE_VERSION configured:
    C++ compiler ...... $CXX
    C++ standard ...... $ax_std
    std::expected ..... $have_std_expected
    std::print ........ $have_std_print
    libarchive ........ $tapir_la_info
    libarchive libs ... $LIBARCHIVE_LIBS
    libfuse ........... fuse3  ($FUSE_LIBS)
    nlohmann/json ..... ${JSON_CFLAGS:-(default include path)}
    libcrypto ......... $CRYPTO_LIBS
])
