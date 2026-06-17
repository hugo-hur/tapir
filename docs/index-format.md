# tapir index format

## Tape layout

A tapir tape is a sequence of tape files (separated by filemarks). Each write
session appends a **data archive** followed immediately by a **manifest archive**:

```
tape file 0  — data tar         (first write session)
tape file 1  — manifest tar     (cumulative index after session 1)
tape file 2  — data tar         (second write session)
tape file 3  — manifest tar     (cumulative index after session 2)
...
```

The manifest is always **cumulative** — it records every file on the tape up to
that point, not just the most recent session. The last manifest on the tape is
the authoritative index. On WORM tapes all previous manifests remain physically
intact and can be recovered with `tfsck --rollback-to`.

If a write session has no new data files (e.g. `mktapir <device>` on a blank
tape), only the manifest tape file is written; no data tape file precedes it.

## Manifest container

The manifest archive is a standard **PAX-restricted tar** (POSIX.1-2001)
containing exactly one member:

```
manifest.json    — the full index as UTF-8 JSON
```

The archive is written with a configurable blocking factor (default 512 × 512 =
262 144 bytes per physical block). The data archives may have a different,
independently auto-detected blocking factor.

Reading uses `archive_read_support_format_all` so any standard tar variant
(V7, ustar, GNU, pax, star) is also accepted.

## manifest.json structure

The top level is a **JSON array** of archive groups, one per data tape file:

```json
[
  [ <archive-header>, <file-entry>, <file-entry>, ... ],
  [ <archive-header>, <file-entry>, ... ],
  ...
]
```

Groups are ordered by `data_tape_file` (ascending). Each group is itself an
array whose first element is the archive header and whose remaining elements
are file entries.

### Archive header (element 0 of each group)

| Field | Type | Description |
|-------|------|-------------|
| `index` | integer | Sequential position of this group in the array (0-based) |
| `data_tape_file` | integer | Tape file number holding this group's data tar |
| `manifest_tape_file` | integer | Tape file number of the manifest that first recorded this archive |
| `volume_uuid` | string | Random v4 UUID, constant for the lifetime of the tape; generated on first write |
| `write_generation` | integer | Monotonically increasing counter; bumped each write session |
| `block_factor` | integer | Blocking factor of the data tar (physical block = `block_factor × 512` bytes) |
| `file_count` | integer | Number of file entries in this group |
| `total_bytes` | integer | Sum of `size` across all file entries |
| `source` | string | Free-form provenance string (empty if not set) |
| `created` | string | ISO-8601 UTC timestamp of when this archive was first indexed |

Example:

```json
{
  "index": 0,
  "data_tape_file": 0,
  "manifest_tape_file": 1,
  "volume_uuid": "f47ac10b-58cc-4372-a567-0e02b2c3d479",
  "write_generation": 1,
  "block_factor": 512,
  "file_count": 3,
  "total_bytes": 1048576,
  "source": "",
  "created": "2026-06-17T19:45:00"
}
```

### File entry (elements 1..N of each group)

| Field | Type | Description |
|-------|------|-------------|
| `path` | string | POSIX path relative to the tape root; no leading `/` or `./`; components separated by `/` |
| `size` | integer | File size in bytes |
| `mtime` | integer | Modification time as a Unix timestamp (seconds since epoch); `0` if unknown |
| `hashes` | object | Map of hash algorithm name to lowercase hex digest; empty object if no hash recorded |
| `verified_with` | null | Reserved — will record the algorithm used on last successful `tfsck` run |

The only hash algorithm currently written is `sha256sum`:

```json
{
  "path": "2022/video.mp4",
  "size": 335909418,
  "mtime": 1655000000,
  "hashes": { "sha256sum": "6944f3cd...4140e" },
  "verified_with": null
}
```

Paths are normalised on import: any leading `./` or `/` is stripped, and path
separators are collapsed. The path stored is exactly the key used for FUSE
lookup.

## Complete example

A tape with two write sessions and three files total:

```json
[
  [
    {
      "index": 0,
      "data_tape_file": 0,
      "manifest_tape_file": 1,
      "volume_uuid": "f47ac10b-58cc-4372-a567-0e02b2c3d479",
      "write_generation": 1,
      "block_factor": 512,
      "file_count": 2,
      "total_bytes": 2097152,
      "source": "",
      "created": "2026-06-17T10:00:00"
    },
    {
      "path": "photos/holiday.jpg",
      "size": 1048576,
      "mtime": 1718611200,
      "hashes": { "sha256sum": "aabbcc..." },
      "verified_with": null
    },
    {
      "path": "photos/portrait.jpg",
      "size": 1048576,
      "mtime": 1718611260,
      "hashes": { "sha256sum": "ddeeff..." },
      "verified_with": null
    }
  ],
  [
    {
      "index": 1,
      "data_tape_file": 2,
      "manifest_tape_file": 3,
      "volume_uuid": "f47ac10b-58cc-4372-a567-0e02b2c3d479",
      "write_generation": 2,
      "block_factor": 256,
      "file_count": 1,
      "total_bytes": 335909418,
      "source": "",
      "created": "2026-06-18T09:00:00"
    },
    {
      "path": "video/film.mp4",
      "size": 335909418,
      "mtime": 1718697600,
      "hashes": { "sha256sum": "112233..." },
      "verified_with": null
    }
  ]
]
```

The manifest at tape file 3 covers both `data_tape_file` 0 and 2. A reader only
needs to read the last manifest to reconstruct the full file tree.

## Generation and volume identity

`volume_uuid` is generated once per tape on the first `mktapir` or `tapir`
write and never changes. It identifies the tape across all its manifests.

`write_generation` is per-archive (not per-manifest). Each new data tape file
gets `max(all existing generations) + 1`. Old archives keep their original
generation number in every subsequent cumulative manifest. `tfsck
--list-generations` lists all generations found on tape; `tfsck --rollback-to
<N>` reinstates an older manifest verbatim as a new tape file at EOD.

## Deletions

`tapir unlink` removes the file from the in-memory index and writes a new
manifest without the entry. The physical data on the tape is never touched —
the bytes remain in the original data tar. On WORM tapes this is the only
possible kind of delete.
