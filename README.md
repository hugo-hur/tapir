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

- Reads the latest manifest from EOT (identified by the tapir PAX magic — see
  *Index identification* below), serves the file tree via FUSE3. Refuses to mount
  a tape whose index predates the magic until it is converted with
  `tfsck --upgrade-manifest`.
- **Append**: copy files in; they are staged in a temp file and written to tape
  asynchronously. All files written between two syncs are batched into a single
  multi-member tar at EOD. `sync` (or unmount) closes that tar and writes a fresh
  cumulative index as the next tape file.
- **Delete**: index-only — the data remains on tape (WORM-safe).
- **mtime**: preserved through the tar header; `touch`/`mv -p` work correctly.
- **permissions**: the tar header mode is preserved and reported by `stat`.
  Sealed (already-on-tape) files display with their write bits masked off
  (read/execute kept), reflecting their immutability; `chmod` succeeds only on a
  file still being written and bakes the new mode into its tar header at flush.
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
- No `-f`: scans every data tape file (refuses if a tapir index already exists,
  since a rescan would resurrect index-deleted files whose data is still on tape;
  use `tfsck` for recovery instead).
- `-f 0,2,5`: index only the listed tape files and merge into the existing index.
  Refuses if one of the listed files is itself a tapir index.
- When the same path occurs more than once across the imported tape files, the
  **last occurrence wins** (matching `tar -x` over an appended archive); the
  earlier copies stay on tape as orphaned data.
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
tfsck <device-nst> [-b N] [-m N] [-v]
```

Streams every indexed data tape file, recomputes SHA-256, and reports
OK / FAIL / ORPHAN (deleted-but-retained) per file. `-b` is the data block
factor, `-m` the manifest block factor. `-v` prints each member's name and
physical-block offset on header read, then the result after hashing.

Verify doubles as a re-indexer: any manifest entry missing its per-member block
offset (an index written before block tracking, or built by `mktapir import`)
has the offset filled in from the live scan. If any were filled, a refreshed
manifest is written at EOD so later mounts get fast per-member seeking.

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

**Roll back to the previous index** (one-step shortcut):

```sh
tfsck --rollback <device-nst>
```

Writes a copy of the second-to-last manifest at EOD, making the previous
generation authoritative again — the quick "undo my last session" form of
`--rollback-to`. Refuses if there are fewer than two manifests on tape.

**Upgrade a pre-magic manifest**:

```sh
tfsck --upgrade-manifest <device-nst>
```

Reads the manifest at the end of tape even if it predates the tapir PAX magic
header (filename match only) and rewrites it at EOD with the magic added. This
is the conversion step required before `tapir` will mount a tape whose index was
written by an older build (see *Index identification* below).

All `tfsck` modes reject unknown options instead of silently ignoring them.

---

## Tape layout and index format

```
tape file 0  — data tar (N members — all files written before first sync)
tape file 1  — manifest tar      (cumulative index)
tape file 2  — data tar (M members — all files written before second sync)
tape file 3  — manifest tar      (cumulative — covers all sessions)
...
```

**Write batching:** everything written between two `sync` calls (or between mount
and the first sync) is accumulated into a single multi-member PAX tar. `sync`
closes the tar (writing end-of-archive blocks + the filemark) and immediately
appends the manifest as the next tape file. This keeps the filemark count low and
lets `tfsck` stream the tape without rewinding between files.

**Per-member seeking:** the manifest records each file's tape file number and its
physical-block offset within that tape file (`tape_block`). On read, `tapir` uses
FSF to reach the tape file and MTFSR to skip directly to the member's tar header
block — no need to read past preceding members in the same tar.

Each manifest is cumulative and supersedes the previous one. On WORM tapes all
prior manifests are preserved and recoverable via `tfsck --rollback` /
`--rollback-to`. The per-file record also carries the tar header mode (`perm`).
See [docs/index-format.md](docs/index-format.md) for the full JSON schema.
Manifest tar is always the last file on tape, so mounting is fast (eod → back 1 file → read).

**Index identification:** every manifest member carries a PAX extended-header
xattr `user.tapir.magic = tapir-index-v1`. `tapir` and `tfsck` treat a tape file
as a tapir index only when its sole member is `manifest.json` **and** carries
this magic. A plain data tar that happens to contain a `manifest.json` is
therefore never mistaken for an index, and a foreign tar appended after the index
does not silently shadow it — it simply fails the magic check. Indexes written
before the magic existed are rejected on mount; convert them in place with
`tfsck --upgrade-manifest`.

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
