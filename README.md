![tapir logo](docs/tapir_logo.png)

# tapir (TApe Physical-Index aRchive)

A FUSE filesystem and tools for browsing, appending to, and verifying **tar tape
archives** on LTO tape drives — including WORM cartridges.

The tape layout is simple: a data tar at tape file N followed by a cumulative
`manifest.json` index tar at tape file N+1. Metadata (`ls`, `stat`) is served
from the in-RAM index; content is read back through libarchive on demand.

Inspired by LTFS but without partitioned tapes or a custom on-tape format.
Works on any tape that Linux's `st` driver can address, including LTO WORM.

> **Why I made this.** I have a load of old LTO-3 tapes here at home and wanted an
> easy way to actually use them — to browse a tape and pull files off it like a
> normal filesystem, instead of juggling `mt` seeks and `tar` by hand. tapir is
> that: index the tape once, then mount it and `ls`/`cp`.

---

## Usage and examples

The tools are built around a shared static library **libtapir** (cf. LTFS's
`libltfs`). A typical end-to-end session with an existing tape of tar archives:

```sh
# 1. Index the tar archives already on the tape (one forward scan):
mktapir import /dev/nst0

# 2. Mount the tape as a read/append filesystem:
mkdir -p /mnt/tape
tapir /dev/nst0 /mnt/tape

ls -l /mnt/tape            # served from the in-RAM index (instant)
cp /mnt/tape/report.pdf .  # streamed from tape on demand
cp newfile.txt /mnt/tape   # staged now, written back to tape on sync/unmount

sync /mnt/tape             # flush staged files + a fresh index to tape
fusermount3 -u /mnt/tape   # unmount (also flushes)

# 3. Later, check everything still matches its checksums:
tfsck /dev/nst0
```

`/dev/nst0` is the no-rewind tape device; substitute your own (e.g. a stable
`/dev/tape/by-id/...-nst` symlink).

### `tapir` — FUSE mount

```sh
tapir <device-nst> <mountpoint> [-b N] [fuse options]
fusermount3 -u <mountpoint>
```

- Reads the latest manifest from EOT (identified by the tapir PAX magic — see
  *Tape layout and index format* below), serves the file tree via FUSE3. Refuses
  to mount a tape whose index predates the magic until it is converted with
  `tfsck --upgrade-manifest`.
- **Append**: copy files in; they are staged in a temp file and written to tape
  asynchronously. All files written between two syncs are batched into a single
  multi-member tar at EOD. `sync` (or unmount) closes that tar and writes a fresh
  cumulative index as the next tape file. (See *Write-back caching* for the full
  staging/flush model.)
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

**Stream files onto tape** (append):

```sh
# Re-stream an existing tar archive from disk:
mktapir append <device-nst> <file.tar> [-b <block-factor>] [-m <manifest-bf>] [-v]

# Create a tape file from disk paths listed on stdin (one per line):
find /data -type f | mktapir append <device-nst> [-b <block-factor>] [-m <manifest-bf>] [-v]

# Same, but read paths from a file instead of stdin:
mktapir append <device-nst> -T <filelist> [-b <block-factor>] [-m <manifest-bf>] [-v]
```

Three input modes — all write a new tape file at EOD and update the index:

- **`<file.tar>`** — re-streams the tar onto tape; each member inside is indexed
  individually (name, size, SHA-256, block position). Accepts all compression formats
  (`.tar.gz`, `.tar.xz`, `.tar.zst`, …) and all tar variants (V7, ustar, GNU, pax).
- **No filename (stdin default)** — reads one absolute or relative disk path per line
  from standard input and creates a new tar from those files. Each file is stored and
  indexed as an individual member; the leading `/` is stripped from the archive member
  name. Non-regular-file entries (directories, symlinks) are written as header-only
  members. Files that cannot be opened emit a warning and are skipped non-fatally.
- **`-T <filelist>`** — identical to stdin mode but reads paths from a file.

> **Important distinction:** passing a `.tar` file via stdin or `-T` archives it as an
> *opaque blob* — it appears in the FUSE mount as a single file `path/to/archive.tar`.
> Only the positional `<file.tar>` argument extracts and indexes the tar's members.

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
written by an older build (see *Tape layout and index format* below).

All `tfsck` modes reject unknown options instead of silently ignoring them.

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

A custom or forked libarchive can be selected with `--with-libarchive`:

```sh
# Use a pre-built fork at a specific path:
./configure --with-libarchive=/opt/libarchive-fork

# Build the bundled fork from the deps/libarchive submodule:
git submodule update --init deps/libarchive
./configure --with-libarchive=bundled

# Force the system libarchive (error if archive_write_header_position absent):
./configure --with-libarchive=system
```

By default `./configure` probes the system libarchive for
`archive_write_header_position()` (the API that lets `mktapir append` record
exact per-member block offsets in a single write pass). If it is absent and
`deps/libarchive` is initialised, the bundled fork is built automatically as a
static library and linked in; the system libarchive is not used at runtime.

The bundled fork lives at `deps/libarchive` (git submodule, branch
`HeaderPositionExtract`, `https://github.com/hugo-hur/libarchive`).
`make dist` bootstraps and embeds the submodule source so release tarballs
are self-contained.

---

## In-depth information

How the filesystem behaves under the hood — durability model, on-tape format, and
WORM handling.

### Write-back caching, staging, and read-after-write

`tapir`'s write path is a **write-back (write-behind) cache** in front of the
tape: writes are acknowledged as soon as they reach RAM, and the slow tape is
updated asynchronously.

- **Staging.** A newly written file is buffered in an anonymous temp file under
  `/tmp`, created with `mkstemp` + an immediate `unlink` — it keeps an open file
  descriptor but no directory entry, so it has no name anything else can reach and
  it self-destructs when the fd closes (including on crash). On most systems
  `/tmp` is `tmpfs`, so staged data lives in RAM/swap rather than on disk.
- **Asynchronous flush.** `close()` returns immediately, handing the staged file
  to a background writer thread, which streams the bytes into the open data tar at
  end-of-tape. The **manifest write is the commit point** — only `fsync`, `sync`,
  or unmount closes that tar and writes the new cumulative index.
- **Read-after-write.** While a just-closed file is still staged (data not yet on
  tape, or on tape but not yet committed by a manifest), `read()` serves it
  straight from the temp fd. Members already read back *from* tape are held in a
  separate in-RAM read cache.
- **Concurrency.** The staged temp fd is shared between the FUSE reader and the
  writer thread; both use positional `pread` (explicit offset, never touching the
  shared file position), so a file can be read at the same instant it is being
  streamed to tape without either side disturbing the other.

Consequences of the write-back model worth knowing:

- **Durability follows `fsync`.** Data written but not yet `fsync`/`sync`/
  unmounted is not durably indexed. After a crash, any bytes the writer already
  streamed sit past the last valid manifest in an unterminated tape file —
  invisible on the next mount (the previous good manifest loads) and recoverable,
  if at all, only via `tfsck` / `mktapir import`. This is the standard write-back
  trade-off, with the manifest as the commit record.
- **No periodic or pressure-driven flush.** Unlike the kernel page cache, there is
  no timer- or memory-pressure-triggered write-back and no dirty-ratio
  backpressure. Dirty data is flushed only on the events above, and the writer
  queue is unbounded — a producer faster than the drive grows the queue and the
  set of open staged temp files, bounded only by `/tmp` capacity.
- **Append-only backing store.** The tape is never overwritten in place.
  Overwriting a file appends a new copy and a new manifest generation (last write
  wins in the index); deletes are index-only. Superseded/orphaned data lingers and
  is recoverable via `tfsck --rollback` / `--rollback-to`. In effect tapir is a
  log-structured, generationed store with a write-back front end.

### Tape layout and index format

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

**Per-member seeking:** the manifest records each file's tape file number plus the
header's location within that tape file — a physical block (`tape_block`) and the
header's byte offset inside that block (`tape_block_offset`). On read, `tapir` FSFs
to the tape file, MTFSRs to the block, reads that whole block, and hands libarchive
the bytes from the header offset onward — so it lands on any member, not just the
first in a multi-member tar. If the offset is unrecorded it falls back to a correct
(slower) full-file scan.

> **`mktapir append` records exact offsets** — `archive_write_header_position()`
> (from the bundled libarchive fork) is called immediately after each
> `archive_write_header()`, giving the header's byte position in the write stream.
> Both the block number and the within-block offset are stored in the index, so any
> file appended via `mktapir append` is directly seekable from the first mount,
> with no `tfsck` pass needed.
>
> **TODO — FUSE writer-side offset:** files written through the FUSE mount (staged
> and flushed by the background writer thread) currently record `tape_block` but
> leave `tape_block_offset` unset. Those members read via the slower full-file scan
> until a `tfsck` pass fills the exact offsets. Fixing this requires the same
> `archive_write_header_position()` call in `writer.cpp`.

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

### WORM cartridge support

All writes go through `append()` (or `overwrite_from_start()` for `--force`),
which positions to EOD before opening the device — existing data is never
overwritten. WORM drives enforce this at the hardware level. Reads and backward
positioning (manifest lookup, import scanning) are unrestricted.

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
