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
# Prefer pkg-config for correct flags; fall back to default search paths.
PKG_PROG_PKG_CONFIG
PKG_CHECK_MODULES([LIBARCHIVE], [libarchive >= 3.0.0],
                  [have_libarchive=yes],
                  [have_libarchive=no])

AS_IF([test "x$have_libarchive" != "xyes"], [
  AC_MSG_NOTICE([pkg-config did not provide libarchive; trying default paths])
  AC_LANG_PUSH([C++])
  AC_CHECK_HEADER([archive.h], [],
    [AC_MSG_ERROR([archive.h not found — install libarchive development files (e.g. libarchive-dev)])])
  AC_CHECK_HEADER([archive_entry.h], [],
    [AC_MSG_ERROR([archive_entry.h not found — install libarchive development files])])
  AC_SEARCH_LIBS([archive_read_new], [archive],
    [LIBARCHIVE_LIBS="-larchive"],
    [AC_MSG_ERROR([libarchive shared library not found])])
  AC_LANG_POP([C++])
  LIBARCHIVE_CFLAGS=""
])

# Functional "usable" gate — runs regardless of how the flags were found.
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

AC_MSG_NOTICE([

  $PACKAGE_NAME $PACKAGE_VERSION configured:
    C++ compiler ...... $CXX
    C++ standard ...... $ax_std
    std::expected ..... $have_std_expected
    std::print ........ $have_std_print
    libarchive libs ... $LIBARCHIVE_LIBS
    libfuse ........... fuse3  ($FUSE_LIBS)
    nlohmann/json ..... ${JSON_CFLAGS:-(default include path)}
    libcrypto ......... $CRYPTO_LIBS
])
