# tapefs

Build configuration for the C++ side of the tar-on-tape project.

## Requirements (enforced by `configure`)

- A C++ compiler with working **C++20** support (hard minimum).
- **C++23** is preferred and selected automatically when available, enabling
  **`std::expected`** (error propagation across the FUSE boundary) and
  **`std::print`** (logging). Control with `--enable-cxx23={auto,yes,no}`.
- A usable **libarchive >= 3.0.0** with PAX support.
- **libfuse 3** (>= 3.0) — **required** (no fuse2 fallback).

### Installing the dependencies (Debian/Ubuntu)

```sh
sudo apt-get install libarchive-dev libfuse3-dev fuse3
```

- `libfuse3-dev` provides the fuse3 headers and the `fuse3.pc` that pkg-config
  needs; `fuse3` provides the `fusermount3` mount helper used at runtime. On this
  box the libfuse3 *runtime* libs are already present — only `libfuse3-dev` is
  missing, so `configure` will fail at the libfuse check until it is installed.

## Build

```sh
autoreconf -i          # bootstrap: generates ./configure (one time / after editing configure.ac)
./configure            # checks C++20 + libarchive, fails loudly if either is missing
make
./src/tapefs           # smoke test: prints libarchive version, confirms C++20 compiled in
```

`./configure` accepts the usual overrides, e.g. `./configure CXX=clang++`,
`./configure --enable-cxx23=no` (force the C++20 baseline), or
`PKG_CONFIG_PATH=/opt/lib/pkgconfig ./configure` for a library in a non-standard prefix.

## How the checks work

- **C++ standard** — C++20 is the hard minimum (`m4/ax_cxx_compile_stdcxx_20.m4`,
  vendored so autoconf-archive is *not* required: it probes concepts, `consteval`,
  templated lambdas, `std::span`). C++23 is preferred and tried first via
  `m4/ax_cxx_try_stdcxx_23.m4` (probes `__cpp_if_consteval`); the first working
  `-std` switch is appended to `$CXX`. `--enable-cxx23=no` skips straight to C++20;
  `=yes` makes C++23 (and the two features below) mandatory. On this box C++23 is
  selected automatically (`-std=c++23`); the compiler default is C++17.
- **std::expected / std::print** — only probed in C++23 mode, via real link tests
  (`#include <expected>` / `#include <print>`). Each defines `HAVE_STD_EXPECTED` /
  `HAVE_STD_PRINT` so the code can `#ifdef` to use them with a fallback. Both are
  available with g++ 14 on this box.
- **libarchive** — pkg-config (`libarchive >= 3.0.0`) with a header/library fallback,
  followed by a functional gate that *links* against the writer API used by the project
  and enforces `ARCHIVE_VERSION_NUMBER >= 3000000` at compile time. The functional gate
  is the real backstop: it rejects an old or broken libarchive even if the headers are
  present.
- **libfuse 3** — pkg-config `fuse3 >= 3.0`, required (errors out otherwise). Defines
  `HAVE_FUSE3` and `FUSE_USE_VERSION 31` in `config.h`, which the sources include
  *before* `<fuse.h>`.

Only pkg-config is assumed available at bootstrap time; the C++ std macros are self-contained.
