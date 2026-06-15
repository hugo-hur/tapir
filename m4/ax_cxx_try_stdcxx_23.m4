# ax_cxx_try_stdcxx_23.m4 — *optionally* switch the compiler into C++23 mode.
#
# Non-fatal companion to AX_CXX_COMPILE_STDCXX_20. If a working C++23 mode is
# found, the necessary -std switch is appended to $CXX, HAVE_CXX23 is defined,
# and ACTION-IF-FOUND runs. Otherwise $CXX is left untouched and
# ACTION-IF-NOT-FOUND runs. The probe checks only the *language* level
# (__cpp_if_consteval) so individual library features (<expected>, <print>) can
# be detected separately afterwards.
#
#   AX_CXX_TRY_STDCXX_23([ACTION-IF-FOUND], [ACTION-IF-NOT-FOUND])

AC_DEFUN([AX_CXX_TRY_STDCXX_23], [dnl
  m4_define([_AX_CXX23_TESTBODY], [[
    #include <version>
    #if !defined(__cpp_if_consteval) || __cpp_if_consteval < 202106L
    #  error "C++23 language support not enabled"
    #endif
    int main() { return 0; }
  ]])

  AC_LANG_PUSH([C++])
  ax_cxx23_ok=no

  AC_CACHE_CHECK([whether $CXX supports C++23 out of the box],
    [ax_cv_cxx_compile_cxx23],
    [AC_COMPILE_IFELSE([AC_LANG_SOURCE([_AX_CXX23_TESTBODY])],
       [ax_cv_cxx_compile_cxx23=yes],
       [ax_cv_cxx_compile_cxx23=no])])
  AS_IF([test "x$ax_cv_cxx_compile_cxx23" = "xyes"], [ax_cxx23_ok=yes])

  AS_IF([test "x$ax_cxx23_ok" = "xno"], [
    for ax_switch in -std=c++23 -std=gnu++23 -std=c++2b -std=gnu++2b; do
      ax_cachevar=AS_TR_SH([ax_cv_cxx_compile_cxx23_$ax_switch])
      AC_CACHE_CHECK([whether $CXX supports C++23 with $ax_switch],
        [$ax_cachevar],
        [ax_save_CXX="$CXX"
         CXX="$CXX $ax_switch"
         AC_COMPILE_IFELSE([AC_LANG_SOURCE([_AX_CXX23_TESTBODY])],
           [eval $ax_cachevar=yes],
           [eval $ax_cachevar=no])
         CXX="$ax_save_CXX"])
      AS_IF([eval test \"x\$$ax_cachevar\" = "xyes"],
        [CXX="$CXX $ax_switch"; ax_cxx23_ok=yes; break])
    done
  ])

  AC_LANG_POP([C++])

  AS_IF([test "x$ax_cxx23_ok" = "xyes"],
    [AC_DEFINE([HAVE_CXX23], [1], [Define if the compiler is in C++23 mode.])
     $1],
    [$2])
])
