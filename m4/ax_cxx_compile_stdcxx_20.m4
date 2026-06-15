# ax_cxx_compile_stdcxx_20.m4 — require a C++20-capable compiler.
#
# Self-contained (does not depend on autoconf-archive being installed).
# On success, appends the necessary -std switch to $CXX and defines HAVE_CXX20.
# On failure, aborts configure with an error.
#
# Strict ISO mode is preferred (-std=c++20) over the GNU-extension mode
# (-std=gnu++20); the older c++2a spellings are tried as a fallback for
# pre-GCC-10 / pre-Clang-10 compilers.

AC_DEFUN([AX_CXX_COMPILE_STDCXX_20], [dnl
  m4_define([_AX_CXX20_TESTBODY], [[
    #include <version>
    #include <concepts>
    #include <span>

    template <typename T>
    concept Addable = requires(T a, T b) { { a + b } -> std::convertible_to<T>; };

    consteval int square(int x) { return x * x; }

    template <Addable T> T add(T a, T b) { return a + b; }

    static_assert(square(5) == 25, "consteval must evaluate at compile time");

    int main() {
      int s = add(2, 3);
      auto lam = [v = s]<typename U>(U u) { return v + u; };   // C++20 templated lambda
      std::span<const int> sp{&s, 1};
      return (lam(40) == 45 && sp.size() == 1) ? 0 : 1;
    }
  ]])

  AC_LANG_PUSH([C++])
  ax_cxx20_success=no

  AC_CACHE_CHECK([whether $CXX supports C++20 out of the box],
    [ax_cv_cxx_compile_cxx20],
    [AC_COMPILE_IFELSE([AC_LANG_SOURCE([_AX_CXX20_TESTBODY])],
       [ax_cv_cxx_compile_cxx20=yes],
       [ax_cv_cxx_compile_cxx20=no])])
  AS_IF([test "x$ax_cv_cxx_compile_cxx20" = "xyes"], [ax_cxx20_success=yes])

  AS_IF([test "x$ax_cxx20_success" = "xno"], [
    for ax_switch in -std=c++20 -std=gnu++20 -std=c++2a -std=gnu++2a; do
      ax_cachevar=AS_TR_SH([ax_cv_cxx_compile_cxx20_$ax_switch])
      AC_CACHE_CHECK([whether $CXX supports C++20 with $ax_switch],
        [$ax_cachevar],
        [ax_save_CXX="$CXX"
         CXX="$CXX $ax_switch"
         AC_COMPILE_IFELSE([AC_LANG_SOURCE([_AX_CXX20_TESTBODY])],
           [eval $ax_cachevar=yes],
           [eval $ax_cachevar=no])
         CXX="$ax_save_CXX"])
      AS_IF([eval test \"x\$$ax_cachevar\" = "xyes"],
        [CXX="$CXX $ax_switch"; ax_cxx20_success=yes; break])
    done
  ])

  AC_LANG_POP([C++])

  AS_IF([test "x$ax_cxx20_success" = "xno"],
    [AC_MSG_ERROR([*** A compiler with working C++20 support is required.])],
    [AC_DEFINE([HAVE_CXX20], [1], [Define if the compiler supports C++20.])])
])
