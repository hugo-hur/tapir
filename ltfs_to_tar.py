#!/usr/bin/env python3
"""
ltfs_to_tar.py — Copy an LTFS mount to a tar tape archive in tape-block order.

Single-pass: SHA256 is computed while each file streams into the tar — the source
tape is read exactly once.  No intermediate disk copy needed.

If ltfs.hash.* xattrs exist on source files (written by ltfs_ordered_copy
--store-hash[-all]) they are verified against the computed digest as each file is
copied. sha256sum is the preferred/default; if it is absent the verifier falls back
to whichever other standard LTFS hashtype is present (sha512sum, sha1sum, md5sum,
crc32sum). Mismatches are reported immediately and summarised at the end.

The file listing + hashes is written as a SEPARATE tar (containing manifest.json)
in its own tape file after the data archive. On tape this is fast to fetch — skip
the data archive at drive speed with `mt fsf 1`, then read just the small manifest
tar, instead of streaming the whole multi-hundred-GB archive to reach a trailing
member.

Writing never reads the tape back — verification is a SEPARATE pass (--verify-only),
so a write isn't slowed by re-traversing the archive across tape seek gaps.

Usage:
    ltfs_to_tar.py <ltfs_mount> [folder ...] <tar_dest> [-b N]   # write/append
    ltfs_to_tar.py --verify-only <tar_dest> [-b N]               # verify, no write

    ltfs_mount   mounted LTFS path            e.g. /lto5
    folder ...   one or more subfolders under the mount to archive recursively
                 (e.g. 2008 2009 2010). Omit to archive the entire mount.
    tar_dest     tape nst device or file      e.g. /dev/tape/by-id/scsi-HU113367G6-nst
                 (last argument, rsync-style)
    --verify-only  read each archive back from the tape, recompute every file's
                 checksum and match it against the manifest (no writing). Reads
                 every byte, so on a tape with large seek gaps it is slow
                 (≈ one near-full-tape pass per archive), but it is a true
                 content check.
    -b / --block-factor
                 512-byte records per tape block (default 512 = 256 KB)

Tape layout (this run writes two consecutive tape files, N and N+1):
    tape file N  :  <year>/filename.mp4 ...   data, in LTFS physical tape-block order
    [filemark]
    tape file N+1:  manifest.json             cumulative tape index (see below)

By default the script APPENDS at end-of-data (mt eod): N is the first free tape
file and existing archives are preserved. Before writing, it reads the previous
manifest (tape file N-1) and carries its entries forward, so the newest manifest
indexes the WHOLE tape. Pass --overwrite to rewind to BOT and write from the start
(N=0), discarding prior tape contents and starting the index fresh. N is printed
when the archive is written.

manifest.json is an array of arrays — one inner array per archive on the tape:
    [
      [ {index,data_tape_file,manifest_tape_file,source,created,file_count,total_bytes},
        {path,size,hashes,verified_with}, ... ],   # archive 0
      [ {…header…}, {file}, … ],                   # archive 1
      …
    ]
Element 0 of each inner array is the summary header (notes its tape file numbers);
the rest are that archive's file records.

To fetch the manifest from tape (use the N+1 printed at write time):
    mt -f <dev> rewind && mt -f <dev> fsf <N+1> && tar -xOf <dev> manifest.json

For a non-tape file dest, the manifest tar is written to <dest>.manifest.tar
(the file dest is always truncated; --overwrite has no effect there).
"""

import argparse
import hashlib
import io
import json
import os
import re
import subprocess
import sys
import tarfile
import time
import zlib


# ── LTFS xattr names ──────────────────────────────────────────────────────────
XATTR_PARTITION   = 'user.ltfs.partition'
XATTR_STARTBLOCK  = 'user.ltfs.startblock'
XATTR_HASH_PREFIX = 'user.ltfs.hash.'   # ltfs.hash.<hashtype>, written by ltfs_ordered_copy


# ── standard LTFS hash types (Format Spec 2.4 Annex F, Table F.1) ─────────────
class _CRC32:
    """hashlib-style adapter for zlib.crc32 (8-char hex, like the spec's crc32sum)."""
    def __init__(self):
        self._c = 0
    def update(self, b):
        self._c = zlib.crc32(b, self._c)
    def hexdigest(self):
        return '%08x' % (self._c & 0xffffffff)


# hashtype → factory of an object with .update(bytes)/.hexdigest()
HASH_FACTORIES = {
    'crc32sum':  _CRC32,
    'md5sum':    hashlib.md5,
    'sha1sum':   hashlib.sha1,
    'sha256sum': hashlib.sha256,
    'sha512sum': hashlib.sha512,
}
DEFAULT_HASH = 'sha256sum'   # always computed (drives the sha256sums.txt manifest)
# Verification preference: sha256sum first, then fall back to any other present type.
VERIFY_PREFERENCE = ['sha256sum', 'sha512sum', 'sha1sum', 'md5sum', 'crc32sum']


def read_stored_hashes(path):
    """Return {hashtype: stored_hex} for every standard ltfs.hash.* xattr present."""
    out = {}
    for ht in HASH_FACTORIES:
        try:
            out[ht] = os.getxattr(path, XATTR_HASH_PREFIX + ht).decode().strip()
        except OSError:
            pass
    return out


# ── file ordering ─────────────────────────────────────────────────────────────
def block_ordered_files(mount, folders=None, exclude=frozenset({'yt-dlp-archive.txt'})):
    """Walk the selected folders; return absolute paths sorted by
    (ltfs.partition, ltfs.startblock) — i.e. LTFS physical tape-block order.

    folders: iterable of subfolder paths relative to mount, each walked
    recursively. None or empty → walk the entire mount. Overlapping folder
    args (e.g. a parent and one of its children) are de-duplicated.
    Paths are gathered from every folder and sorted together so the tape head
    moves strictly forward across the whole archive, not per-folder.
    """
    if folders:
        roots = [os.path.join(mount, f.strip('/')) for f in folders]
    else:
        roots = [mount]
    items = []
    seen = set()
    for base in roots:
        if not os.path.isdir(base):
            print(f'  WARNING: not a directory, skipping: {base}', file=sys.stderr)
            continue
        for root, _dirs, files in os.walk(base):
            for fname in files:
                if fname in exclude:
                    continue
                p = os.path.join(root, fname)
                if p in seen:
                    continue
                seen.add(p)
                try:
                    part = os.getxattr(p, XATTR_PARTITION).decode()
                    sb   = int(os.getxattr(p, XATTR_STARTBLOCK).decode())
                except OSError:
                    part, sb = 'z', 1 << 62   # no xattr → sort to end
                items.append((part, sb, p))
    items.sort(key=lambda x: (x[0], x[1]))
    return [p for *_, p in items]


# ── hash-while-reading ────────────────────────────────────────────────────────
class HashingReader:
    """File-like read wrapper feeding data through one or more hashers as tarfile
    pulls it — so several digests are produced in the single read pass."""

    def __init__(self, fobj, size, hashtypes):
        self._f       = fobj
        self._remain  = size
        self._hashers = {ht: HASH_FACTORIES[ht]() for ht in hashtypes}

    def read(self, n=-1):
        if n < 0 or n > self._remain:
            n = self._remain
        if n == 0:
            return b''
        chunk = self._f.read(n)
        if chunk:
            for h in self._hashers.values():
                h.update(chunk)
            self._remain -= len(chunk)
        return chunk

    def digests(self):
        """{hashtype: hexdigest} for every hasher this reader was built with."""
        return {ht: h.hexdigest() for ht, h in self._hashers.items()}


# ── block-aligned tape writer ─────────────────────────────────────────────────
class BlockWriter:
    """
    Wraps a tape/file descriptor; buffers writes and flushes in exact multiples of
    block_size so the tape driver always receives full, aligned blocks.
    """

    def __init__(self, path, block_size):
        self._fd    = open(path, 'wb', buffering=0)   # unbuffered → kernel sees our writes
        self._bsz   = block_size
        self._buf   = bytearray()
        self.bytes_written = 0

    def write(self, data):
        self._buf.extend(data)
        while len(self._buf) >= self._bsz:
            self._fd.write(bytes(self._buf[:self._bsz]))
            del self._buf[:self._bsz]
            self.bytes_written += self._bsz
        return len(data)

    def flush(self):
        pass   # only flush on close; mid-archive flushes would create short blocks

    def close(self):
        if self._buf:
            # pad final partial block with NUL (standard tar end-of-archive convention)
            pad = (-len(self._buf)) % self._bsz
            self._buf.extend(b'\0' * pad)
            self._fd.write(bytes(self._buf))
            self.bytes_written += len(self._buf)
            self._buf.clear()
        self._fd.close()


# ── block-aligned tape reader ─────────────────────────────────────────────────
class BlockReader:
    """Read wrapper for tarfile streaming mode: pulls full block_size records off
    the tape/file and serves tarfile's smaller read() requests from the buffer.
    Tape devices must be read one whole block per read() call, so this buffering
    is required (a short read() on a large tape block fails with ENOMEM)."""

    def __init__(self, path, block_size):
        self._fd  = open(path, 'rb', buffering=0)
        self._bsz = block_size
        self._buf = bytearray()
        self._eof = False

    def _fill(self, need):
        while (need is None or len(self._buf) < need) and not self._eof:
            chunk = self._fd.read(self._bsz)
            if not chunk:           # 0 bytes = filemark / EOF on a tape device
                self._eof = True
            else:
                self._buf.extend(chunk)

    def read(self, n=-1):
        if n is None or n < 0:
            self._fill(None)
            out = bytes(self._buf); self._buf.clear(); return out
        self._fill(n)
        out = bytes(self._buf[:n]); del self._buf[:n]; return out

    def close(self):
        self._fd.close()


# ── helpers ───────────────────────────────────────────────────────────────────
def rewind(device):
    subprocess.run(['mt', '-f', device, 'rewind'], check=True)


def eod(device):
    """Position at end-of-data so the next write appends after existing tape files."""
    subprocess.run(['mt', '-f', device, 'eod'], check=True)


def tape_file_number(device):
    """Current tape file number from `mt status` (0 if blank/unparseable)."""
    out = subprocess.run(['mt', '-f', device, 'status'],
                         capture_output=True, text=True).stdout
    m = re.search(r'[Ff]ile number\s*=\s*(-?\d+)', out)
    return max(int(m.group(1)), 0) if m else 0


def position_file_from_eod(device, files_before_eod):
    """Position at the start of the data file `files_before_eod` files before EOD
    (1 = last file, 2 = second-to-last, …).

    Cheaper than rewind+fsf for files near the end of a full tape: it seeks to EOD
    and spaces back a few filemarks instead of winding to BOT and forward over
    everything. Returns False if the tape is too short for the requested back-space
    (caller should fall back to rewind+fsf).
    """
    eod(device)
    n = tape_file_number(device)            # EOD file number == number of files on tape
    target = n - files_before_eod
    if target < 0:
        return False
    if target == 0:
        rewind(device)                      # first file lives at BOT
        return True
    # Cross (files_before_eod + 1) filemarks back to the BOT-side of the filemark
    # before the target file, then forward over 1 to land at the target's first block.
    subprocess.run(['mt', '-f', device, 'bsf', str(files_before_eod + 1)], check=True)
    subprocess.run(['mt', '-f', device, 'fsf', '1'], check=True)
    return True


def extract_tape_manifest(device, bfactor):
    """tar-extract & parse manifest.json from the CURRENT tape position.

    Returns the parsed JSON (cumulative array-of-arrays index) or None if the file
    at the current position holds no readable manifest. Positioning is the caller's
    job (see position_file_from_eod).
    """
    try:
        out = subprocess.run(
            ['tar', f'--blocking-factor={bfactor}', '-xOf', device, 'manifest.json'],
            capture_output=True)
        if out.returncode != 0 or not out.stdout:
            return None
        return json.loads(out.stdout.decode('utf-8'))
    except (subprocess.SubprocessError, json.JSONDecodeError, OSError, UnicodeDecodeError):
        return None


def write_single_member_tar(dest, name, data, bsize):
    """Write a one-member tar (member `name` holding `data`) to `dest`.

    On a no-rewind tape device this is written at the current position, so calling
    it after the data archive has been closed (a filemark already written) lays the
    manifest down as the next tape file. Returns bytes written.
    """
    writer = BlockWriter(dest, bsize)
    try:
        tf = tarfile.open(fileobj=writer, mode='w|', bufsize=bsize)
        info = tarfile.TarInfo(name=name)
        info.size = len(data)
        tf.addfile(info, io.BytesIO(data))
        tf.close()
    finally:
        writer.close()
    return writer.bytes_written


def fmt_size(n):
    for unit in ('B', 'KB', 'MB', 'GB', 'TB'):
        if abs(n) < 1024:
            return f'{n:.1f} {unit}'
        n /= 1024
    return f'{n:.1f} PB'


# ── verify (separate pass) ──────────────────────────────────────────────────
def verify_tape(dest, bfactor):
    """Separate verification pass (no writing).

    Read the latest cumulative manifest, then seek to each archive's data file,
    read every file back out of the tar, recompute its checksum and match it
    against the hash stored in the manifest. This reads every byte, so on a tape
    with large seek gaps it is slow (≈ one near-full-tape pass per archive), but
    it is a true content check.
    """
    is_tape = dest.startswith('/dev/')
    bsize   = bfactor * 512
    print('=== ltfs_to_tar --verify-only ===')
    print(f'  device : {dest}')
    print(f'  started: {time.strftime("%Y-%m-%d %T")}\n')

    if is_tape:
        if not position_file_from_eod(dest, 1):   # latest manifest = last file
            rewind(dest)
        idx = extract_tape_manifest(dest, bfactor)
    else:
        idx = extract_tape_manifest(dest + '.manifest.tar', bfactor)
    if not isinstance(idx, list):
        sys.exit(f'ERROR: no readable manifest.json on {dest}')

    print(f'  index  : {len(idx)} archive(s)')
    problems = 0
    for arc in idx:
        h     = arc[0]
        want  = h['file_count']
        years = sorted({e['path'].split('/')[0] for e in arc[1:]})
        line  = (f"  #{h['index']} data=file{h['data_tape_file']} "
                 f"manifest=file{h['manifest_tape_file']} years={years} "
                 f"({want} files, {h['total_bytes']/1e9:.2f} GB)")
        # read every file back and match its checksum against the manifest
        print(line + '  — reading + checksumming…')
        if is_tape:
            rewind(dest)
            if h['data_tape_file'] > 0:
                subprocess.run(['mt', '-f', dest, 'fsf', str(h['data_tape_file'])], check=True)
        okc, fails = verify_archive_checksums(dest, bsize, arc)
        problems += len(fails)
        for name, why in fails:
            print(f'      FAIL  {name}: {why}')
        print(f"      → {okc}/{want} files verified"
              + (f", {len(fails)} FAILED" if fails else ", all OK"))
    print()
    if problems:
        sys.exit(f'FAILED: {problems} file(s) failed checksum verification')
    print(f'=== verify OK: {time.strftime("%Y-%m-%d %T")} ===')


def verify_archive_checksums(dest, bsize, arc):
    """Stream the data tar at the current position; recompute each file's checksum
    and compare to the manifest. Returns (ok_count, [(path, reason), …])."""
    want = {e['path']: e['hashes'] for e in arc[1:]}
    seen, ok, fails = set(), 0, []
    reader = BlockReader(dest, bsize)
    try:
        tf = tarfile.open(fileobj=reader, mode='r|', bufsize=bsize)
        for m in tf:
            if not m.isfile():
                continue
            seen.add(m.name)
            stored = want.get(m.name)
            f = tf.extractfile(m)
            if stored is None:
                fails.append((m.name, 'not in manifest'))
                if f:                                   # still drain the member
                    for _ in iter(lambda: f.read(1 << 20), b''):
                        pass
                continue
            ht = 'sha256sum' if 'sha256sum' in stored else next(iter(stored))
            hsh = HASH_FACTORIES[ht]()
            for chunk in iter(lambda: f.read(1 << 20), b''):
                hsh.update(chunk)
            if hsh.hexdigest() == stored[ht]:
                ok += 1
            else:
                fails.append((m.name, f'{ht} mismatch'))
        tf.close()
    finally:
        reader.close()
    for path in want:                                   # in manifest but not on tape
        if path not in seen:
            fails.append((path, 'missing from archive'))
    return ok, fails


# ── main ──────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('ltfs_mount',
                    help='Mounted LTFS path (e.g. /lto5). With --verify-only, pass '
                         'the tape device/file to verify here.')
    ap.add_argument('rest', nargs='*', metavar='[folder ...] tar_dest',
                    help='Optional subfolders to archive recursively, then the tape '
                         'device/file as the LAST argument (rsync-style). Omit folders '
                         'to archive the whole mount.')
    ap.add_argument('-b', '--block-factor', type=int, default=512, metavar='N',
                    help='Tape blocking factor: N × 512 bytes per block (default 512 = 256 KB)')
    ap.add_argument('--overwrite', action='store_true',
                    help='Rewind to BOT and overwrite the tape from the start. '
                         'Default is to append at end-of-data (mt eod), preserving '
                         'existing tape files. (No effect on a non-tape file dest, '
                         'which is always truncated.)')
    ap.add_argument('--verify-only', action='store_true',
                    help='Do not write. Read each archive back from the tape, '
                         "recompute every file's checksum and match it against the "
                         'manifest, then exit. Writing never verifies on its own — '
                         'run this separately. Reads every byte, so it is slow on '
                         'tapes with large seek gaps.')
    args = ap.parse_args()

    bfactor = args.block_factor
    bsize   = bfactor * 512
    positionals = [args.ltfs_mount] + args.rest

    # ── verify-only: separate verification pass, no writing ───────────────────
    if args.verify_only:
        verify_tape(positionals[-1], bfactor)
        return 0

    if len(positionals) < 2:
        sys.exit('ERROR: need <ltfs_mount> [folder ...] <tar_dest>')
    mount   = positionals[0].rstrip('/')
    folders = positionals[1:-1]
    dest    = positionals[-1]
    is_tape = dest.startswith('/dev/')

    print('=== ltfs_to_tar ===')
    print(f'  source      : {mount}')
    print(f'  destination : {dest}')
    if is_tape:
        print(f'  mode        : {"overwrite from BOT" if args.overwrite else "append at end-of-data"}')
    print(f'  block size  : {bfactor} × 512 = {bsize // 1024} KB per block')
    print(f'  started     : {time.strftime("%Y-%m-%d %T")}')
    print()

    # ── [1/3] file list in LTFS physical block order ──────────────────────────
    if folders:
        print(f'  folders     : {", ".join(folders)}')
    else:
        print('  folders     : (entire mount)')
    print('--- [1/3] tape-block-ordered file list ---')
    files = block_ordered_files(mount, folders=folders)
    if not files:
        sys.exit('ERROR: no files found under ' + mount)
    total_bytes = sum(os.path.getsize(p) for p in files)
    print(f'  {len(files)} files  /  {fmt_size(total_bytes)}')
    print()

    # ── [2/3] single-pass: hash while streaming into tar ──────────────────────
    print('--- [2/3] single-pass SHA256 + tar write ---')
    # start_fileno = tape file number where this run's data archive begins.
    # 0 for overwrite/BOT or a blank tape; the first free file when appending.
    # prior_indexes = cumulative array-of-arrays index read from the previous
    # manifest on the tape, so this run's manifest describes the whole tape.
    start_fileno  = 0
    prior_indexes = []
    if is_tape:
        if args.overwrite:
            rewind(dest)
            print('  tape rewound — writing from BOT (tape file 0)')
        else:
            eod(dest)
            start_fileno = tape_file_number(dest)
            print(f'  positioned at end-of-data — appending at tape file {start_fileno}')
            if start_fileno > 0:
                # The previous manifest is the tape file immediately before EOD.
                # Reach it cheaply from EOD (no full rewind); fall back to rewind+fsf.
                if not position_file_from_eod(dest, 1):
                    rewind(dest)
                    if start_fileno - 1 > 0:
                        subprocess.run(['mt', '-f', dest, 'fsf', str(start_fileno - 1)], check=True)
                prev = extract_tape_manifest(dest, bfactor)
                if isinstance(prev, list):
                    prior_indexes = prev
                    nfiles = sum(len(idx) - 1 for idx in prior_indexes)
                    print(f'  read previous index (tape file {start_fileno - 1}): '
                          f'{len(prior_indexes)} archive(s), {nfiles} file(s) already on tape')
                else:
                    print(f'  WARNING: no readable index at tape file {start_fileno - 1}; '
                          f'starting cumulative index fresh (existing data untouched)')
                eod(dest)   # reading moved the head — reposition to append

    manifest   = []     # per-file records for the separate manifest.json
    xattr_ok   = 0
    xattr_fail = []     # (relpath, hashtype, stored_hash, computed_hash)

    writer = BlockWriter(dest, bsize)
    t0 = time.monotonic()
    try:
        tf = tarfile.open(fileobj=writer, mode='w|', bufsize=bsize)

        for i, abspath in enumerate(files, 1):
            relpath = abspath[len(mount) + 1:]   # e.g. "2013/Some Video [id]_h265.mp4"
            fsize   = os.path.getsize(abspath)

            # Read every standard ltfs.hash.* xattr present (ltfs_ordered_copy --store-hash[-all]).
            stored = read_stored_hashes(abspath)
            # Pick the hashtype to verify against: sha256sum (default) if present,
            # otherwise fall back to the best other standard type that is present.
            verify_ht = next((ht for ht in VERIFY_PREFERENCE if ht in stored), None)

            # Always compute the default (for the manifest); add the verify type if different.
            compute = {DEFAULT_HASH}
            if verify_ht:
                compute.add(verify_ht)

            # Stream file through HashingReader → tarfile → BlockWriter → tape
            with open(abspath, 'rb') as fobj:
                reader = HashingReader(fobj, fsize, compute)
                info   = tf.gettarinfo(abspath, arcname=relpath)
                tf.addfile(info, reader)

            digests = reader.digests()
            manifest.append({
                'path':         relpath,
                'size':         fsize,
                'hashes':       digests,            # sha256sum always, + verified type if any
                'verified_with': verify_ht,         # null if no stored hash to check against
            })

            # Verify against the chosen stored xattr hash
            if verify_ht:
                if digests[verify_ht] == stored[verify_ht]:
                    xattr_ok += 1
                    tag = f' {verify_ht}:OK'
                else:
                    xattr_fail.append((relpath, verify_ht, stored[verify_ht], digests[verify_ht]))
                    tag = f' *** {verify_ht} MISMATCH ***'
            else:
                tag = ''

            # Progress line (overwrites in place)
            elapsed = time.monotonic() - t0
            speed   = writer.bytes_written / elapsed / 1048576 if elapsed else 0
            label   = relpath if len(relpath) <= 50 else '…' + relpath[-49:]
            print(f'\r  [{i:>5}/{len(files)}] {speed:6.1f} MB/s  {label}{tag}          ',
                  end='', flush=True)

        print()   # end the \r progress line

        # Close the data archive. On tape this flushes the final block and the st
        # driver writes a filemark, so the manifest below becomes the next tape file.
        tf.close()

    finally:
        writer.close()

    elapsed = time.monotonic() - t0
    print(f'  {fmt_size(writer.bytes_written)} written in {elapsed:.0f}s'
          f'  ({writer.bytes_written / elapsed / 1048576:.1f} MB/s avg)')
    print(f'  done: {time.strftime("%T")}')
    print()

    # ── write the manifest as a separate tar (its own tape file) ──────────────
    # Cumulative tape index: an array of arrays. Each inner array is one archive —
    # element 0 is a summary header (its tape file numbers + counts), followed by
    # that archive's per-file records. Previous archives are carried forward from
    # the index read at append time, so the newest manifest describes the whole tape.
    this_index = [
        {
            'index':              len(prior_indexes),
            'data_tape_file':     start_fileno,
            'manifest_tape_file': start_fileno + 1,
            'source':             mount,
            'created':            time.strftime('%Y-%m-%dT%H:%M:%S'),
            'block_factor':       bfactor,
            'file_count':         len(files),
            'total_bytes':        total_bytes,
        },
        *manifest,
    ]
    cumulative     = prior_indexes + [this_index]
    manifest_bytes = json.dumps(cumulative, ensure_ascii=False).encode('utf-8')
    manifest_dest  = dest if is_tape else dest + '.manifest.tar'
    print('--- [3/3] manifest (separate tar file) ---')
    mbytes = write_single_member_tar(manifest_dest, 'manifest.json', manifest_bytes, bsize)
    if is_tape:
        print(f'  manifest.json ({fmt_size(len(manifest_bytes))}) → tape file {start_fileno + 1}, '
              f'{fmt_size(mbytes)} on tape')
        print('  fetch latest index: mt -f <dev> eod && mt -f <dev> bsf 2 '
              '&& mt -f <dev> fsf 1 && tar -xOf <dev> manifest.json')
    else:
        print(f'  manifest.json ({fmt_size(len(manifest_bytes))}) → {manifest_dest}')
    print()

    # ── xattr verification summary ────────────────────────────────────────────
    if xattr_ok or xattr_fail:
        print('--- xattr hash verification ---')
        print(f'  verified OK  : {xattr_ok}')
        if xattr_fail:
            print(f'  MISMATCHES   : {len(xattr_fail)}')
            for rp, ht, stored, computed in xattr_fail:
                print(f'    {rp}  [{ht}]')
                print(f'      stored   : {stored}')
                print(f'      computed : {computed}')
        print()

    # Writing does NOT read the tape back — verification is a separate pass
    # (it would otherwise re-traverse the whole archive across the seek gaps).
    if is_tape:
        print(f'  verify separately:  ltfs_to_tar.py --verify-only {dest}')
    print(f'=== finished: {time.strftime("%Y-%m-%d %T")} ===')

    if xattr_fail:
        sys.exit(f'FAILED: {len(xattr_fail)} xattr hash mismatch(es)')


if __name__ == '__main__':
    main()
