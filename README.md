![tapir logo](docs/tapir_logo.png)

# tapir (TApe Physical-Index aRchive)

A FUSE filesystem and tools for browsing, appending to, and verifying **tar tape
archives** on LTO tape drives — including WORM cartridges.

The tape layout is simple: a data tar at tape file N followed by a cumulative
`manifest.json` index tar at tape file N+1. Metadata (`ls`, `stat`) is served
from the in-RAM index; content is read back through libarchive on demand.

Inspired by LTFS but without partitioned tapes or a custom on-tape format.
Works on any tape that Linux's `st` driver can address, including LTO WORM.

---

## Tools

Built around a shared static library **libtapir** (cf. LTFS's `libltfs`):

### `tapir` — FUSE mount

```sh
tapir <device-nst> <mountpoint> [-b N] [fuse options]
fusermount3 -u <mountpoint>
```

- Reads the latest manifest from EOT, serves the file tree via FUSE3.
- **Append**: copy files in; they are staged in a temp file and written as a new
  tar at EOD on unmount or `sync`. A fresh cumulative index is written after.
- **Delete**: index-only — the data remains on tape (WORM-safe).
- **mtime**: preserved through the tar header; `touch`/`mv -p` work correctly.
- **fsync**: `sync <mountpoint>` or application `fsync` flushes staged files and
  the index to tape immediately without unmounting.
- `-b N` sets the manifest blocking factor (×512 bytes, default 512).

### `mktapir` — tape initialisation and import

**Initialise a blank tape** (default command):

```sh
mktapir <device-nst> [-m <manifest-block-factor>]
```

Writes a fresh empty tapir index on a blank tape. Refuses if the tape already
has files.

```sh
mktapir <device-nst> --force
```

Rewinds to the start of tape and overwrites with a blank index. On WORM tapes
the drive rejects this if data already exists (hardware enforcement).

**Index existing tar tape files** (import):

```sh
mktapir import <device-nst> [-f <tape-files>] [-b <block-factor>] [-m <manifest-bf>] [-v]
```

Scans existing tar archives on tape and writes a cumulative index at EOD.
- No `-f`: scans every data tape file (refuses if a tapir index already exists —
  a rescan would recover deleted files and pick the wrong version of overwritten
  ones; use `tfsck` for recovery instead).
- `-f 0,2,5`: index only the listed tape files and merge into the existing index.
- Block size is auto-detected per tape file; `-b` is a fallback/override.
- `-v`: prints each filename as soon as its header is read, then SHA-256 + size
  after hashing — useful for progress on large archives.

Reads all tar variants (V7, ustar, GNU, pax, star) via libarchive.

**Stream a tar from disk onto tape** (append):

```sh
mktapir append <device-nst> <file.tar> [-b <block-factor>] [-m <manifest-bf>] [-v]
```

Re-streams a tar from disk into a new tape file at EOD and adds its contents
to the existing index. Accepts compressed archives (`.tar.gz`, `.tar.xz`, etc.).

### `tfsck` — verify and recovery

**Verify** checksums against the index:

```sh
tfsck <device-nst> [-b N] [-v]
```

Streams every indexed data tape file, recomputes SHA-256, and reports
OK / FAIL / ORPHAN (deleted-but-retained) per file. `-v` prints each member
name on header read, then the result after hashing.

**List all index generations on tape**:

```sh
tfsck --list-generations <device-nst>
```

Reads every manifest on tape and prints a table of generation number, volume
UUID, file count, and creation time, with `← current` marking the latest.

**Roll back to an earlier index** (analogous to `ltfsck`):

```sh
tfsck --rollback-to <generation> <device-nst>
```

Finds the manifest with the given write-generation and writes it verbatim as a
new tape file at EOD, making it the new authoritative index. All original data
tape files remain untouched. Refuses if the tape is full.

---

## Tape layout and index format

```
tape file 0  — data tar          (first write session)
tape file 1  — manifest tar      (cumulative index)
tape file 2  — data tar          (second write session)
tape file 3  — manifest tar      (cumulative — covers all sessions)
...
```

Each manifest is cumulative and supersedes the previous one. On WORM tapes all
prior manifests are preserved and recoverable. See
[docs/index-format.md](docs/index-format.md) for the full JSON schema.
Manifest tar is always the last file on tape, so mounting is fast (eod -> back 1 file -> read).

---

## WORM cartridge support

All writes go through `append()` (or `overwrite_from_start()` for `--force`),
which positions to EOD before opening the device — existing data is never
overwritten. WORM drives enforce this at the hardware level. Reads and backward
positioning (manifest lookup, import scanning) are unrestricted.

---

## Requirements

- C++ compiler with **C++20** support (minimum); **C++23** preferred and selected
  automatically when available.
- **libarchive ≥ 3.0.0** with PAX write support.
- **libfuse3 ≥ 3.0** (`tapir` only; no fuse2 fallback).
- **nlohmann/json ≥ 3.0** — manifest parsing.
- **libcrypto** (OpenSSL) — SHA-256.

### Debian / Ubuntu

```sh
sudo apt-get install build-essential libarchive-dev libfuse3-dev fuse3 \
                     nlohmann-json3-dev libssl-dev
```

Tape positioning uses Linux `st` driver ioctls (`<sys/mtio.h>`) directly — no
`mt` binary required at runtime.

---

## Build

```sh
./autogen.sh      # regenerate ./configure (required after checkout or editing configure.ac)
./configure       # checks C++20/23, libarchive, libfuse3, nlohmann/json, libcrypto
make
make check        # run unit tests
```

`./configure` accepts the usual overrides:

```sh
./configure CXX=clang++
./configure --enable-cxx23=no          # force the C++20 baseline
PKG_CONFIG_PATH=/opt/lib/pkgconfig ./configure
```

---

## How the autotools checks work

- **C++ standard** — C++20 is the hard minimum (`m4/ax_cxx_compile_stdcxx_20.m4`,
  vendored so autoconf-archive is not required). C++23 is tried first via
  `m4/ax_cxx_try_stdcxx_23.m4`; `--enable-cxx23=no` skips to C++20, `=yes` makes
  it mandatory.
- **std::expected / std::print** — probed in C++23 mode via real link tests;
  define `HAVE_STD_EXPECTED` / `HAVE_STD_PRINT` for conditional use.
- **libarchive** — pkg-config with a functional link-test gate that rejects old
  or broken installations even if headers are present.
- **libfuse3** — pkg-config `fuse3 ≥ 3.0`, required; defines `FUSE_USE_VERSION 31`
  in `config.h`.
