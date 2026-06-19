// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

// tape.hpp — tape positioning + tape-file tar I/O on the no-rewind device.
//
// Uses the Linux st-driver ioctls for positioning and libarchive (block-aligned)
// for reading/writing whole tape files. Mirrors ltfs_to_tar.py's tape layout: a
// data archive at tape file N, then the cumulative manifest tar at the following
// file. Part of libtapir.
//
// WORM cartridges: all writes go through append(), which positions to EOD
// (MTEOM) before opening the device for writing — existing data is never
// overwritten. WORM drives enforce this at the hardware level anyway, so
// tapir never attempts anything a WORM drive would reject. Reads and backward
// positioning (manifest lookup, import scanning) are unrestricted on all WORM
// drives.
//
// Tar format compatibility: reads use archive_read_support_format_all, so all
// tar variants (V7, ustar, GNU, pax/POSIX.1-2001, star) are accepted — a tape
// written by any standard tool imports cleanly.

#ifndef TAPIR_TAPE_HPP
#define TAPIR_TAPE_HPP

#include "raii.hpp"
#include "tar_io.hpp"

#include <functional>
#include <string>
#include <vector>

namespace tapir
{

    // TODO: add hardware encryption possibility (possibly stenc as a library if it has one)
    // and openssl fallback for AES256-GCM (the same algorithm as the LTO hardware uses), also encrypting the index.
    // In software fallback mode allow only hex or base64 256 bit keys, no weak passphrases (never via argv; keyfile/env).
    // AES-GCM is an authenticated scheme and instantly tells if output is not ok, so key correctness need not be
    // checked separately to avoid emitting corrupted data.
    // The key-derivation (KDF) and per-frame nonce design lives in security.cpp (tapir::security).
    //
    // Also add check to configure.ac that the AES256-GCM features are available in the used openssl library.
    // Also add flag to configure to disable these encryption features.
    class Tape
    {
    public:
        // `device` is the no-rewind node (…-nst); `manifest_block_factor` is the
        // blocking factor (×512) used for the manifest tape files (the -b option).
        Tape(std::string device, int manifest_block_factor)
            : dev_(std::move(device)), mbf_(manifest_block_factor) {}

        // When set, every tape operation (rewind, fsf/bsf, eod, opens, reads) is
        // logged to stderr — useful for the admin tools (mktapir) and debugging.
        void set_verbose(bool v) { verbose_ = v; }

        // Positioning
        bool rewind();
        bool eod();
        bool fsf(int n);
        bool bsf(int n);
        int file_number();       // current tape file number (mt status), or -1 on error
        int64_t block_number();  // current absolute block address (mt_blkno), or -1 on error

        // How to move the tape from file `current` (-1 = position unknown) to file
        // `target`, expressed as: [rewind] then bsf(bsf) then fsf(fsf). Pure (no I/O)
        // so it can be unit-tested. Invariant: it never emits a bsf that would cross
        // beginning-of-tape — seeking to file 0 rewinds instead of bsf-ing past BOT
        // (the bug that broke mktapir import), and bsf is always <= current.
        struct TapeSeek { bool rewind = false; int bsf = 0; int fsf = 0; };
        static TapeSeek plan_seek(int current, int target);

        // End-of-data survey in one pass: returns the number of tape files (or -1
        // on error) and sets `full` if the tape is at end-of-tape — i.e. there is
        // no room to append an index (e.g. a tar that spans onto another tape).
        int survey(bool &full);

        // Read manifest.json from the latest manifest (the last tape file). Requires
        // the tapir PAX magic xattr — rejects tape files that carry manifest.json
        // without it (old-format indexes and unrelated tars). Fast: only the first
        // member is inspected.
        bool read_latest_manifest(std::string &out);

        // Like read_latest_manifest but accepts old-format manifests that lack the
        // magic xattr (filename match only). Used by tfsck --upgrade-manifest and the
        // tapir mount-time warning probe — not for normal operation.
        bool read_latest_manifest_legacy(std::string &out);

        // Read manifest.json from a specific tape file (for generation enumeration
        // and rollback). Returns false if the tape file is not a tapir manifest.
        bool read_manifest_at(int tape_file, std::string &out);

        // Extract one member from the data archive at `tape_file`. When both
        // `block_num` and `block_offset` are >= 0, seeks straight to the member's
        // block and skips the within-block header offset (fast path); otherwise, or
        // if that fast read fails, falls back to scanning the tape file from its
        // start. `block_offset` is the header's byte offset within `block_num`.
        bool read_member(int tape_file, int block_factor, int64_t block_num, int64_t block_offset,
                         const std::string &member, Fd &out_fd, uint64_t &out_size);

        // Stream every member of the data archive at `tape_file`, calling cb per file.
        // on_header(name) fires immediately on header read; cb fires after data is hashed.
        // `block_num` >= 0 uses MTSEEK (same semantics as read_member).
        bool scan_archive(int tape_file, int block_factor, int64_t block_num,
                          const std::function<void(const std::string &, const std::string &,
                                                   uint64_t, time_t, mode_t)> &cb,
                          const std::function<void(const std::string &, bool is_tapir_index)> &on_header = {});

        // Like scan_archive but also reports each member's header position as a
        // (block, offset) pair within the tape file (see tar_for_each_member_with_blocks).
        // Used by tfsck to fill tape_block / tape_block_offset for archives indexed
        // without them (imports, or pre-offset manifests).
        bool scan_archive_with_blocks(
            int tape_file, int block_factor,
            const std::function<void(const std::string &name, int64_t block, int64_t offset,
                                     const std::string &sha256, uint64_t size,
                                     time_t mtime, mode_t mode)> &cb,
            const std::function<void(const std::string &name, int64_t block,
                                     bool is_tapir_index)> &on_header = {});

        // Like scan_archive, but auto-detects the data archive's physical block size
        // (reported via `detected_block_bytes`) by reading with an oversized buffer —
        // a single pass that both scans and discovers the block factor.
        bool scan_archive_detect(int tape_file, int &detected_block_bytes,
                                 const std::function<void(const std::string &, const std::string &,
                                                          uint64_t, time_t, mode_t)> &cb,
                                 const std::function<void(const std::string &, bool is_tapir_index)> &on_header = {});

        // Append a new data archive at EOD (written by `write_data`, or skipped if it
        // is empty/null), then write the manifest as the next tape file.
        // `make_manifest(data_tape_file)` produces the manifest JSON, so the new
        // archive's records can reference the tape file it landed on.
        // `out_data_tape_file` receives that tape file number.
        bool append(int block_factor,
                    const std::function<bool(struct archive *)> &write_data,
                    const std::function<std::string(int data_tape_file)> &make_manifest,
                    int &out_data_tape_file);

        // Primitives used by the background writer thread (tapir). The writer
        // accumulates files into one open tape file between syncs; on sync it
        // closes the tape file and appends a manifest as the next tape file.

        // Open a new tape file at EOD for streaming multi-member writes.
        // The caller receives an open ArchiveWritePtr (write to it with
        // tar_write_files) and the write Fd (needed for mt_blkno queries).
        // Call note_write_done() + close the Fd after archive_write_close().
        bool open_write_at_eod(int block_factor,
                               ArchiveWritePtr &out_ar, Fd &out_fd,
                               int &out_tape_file);

        // Update current_file_ tracking after the caller closes a write Fd.
        void note_write_done(int tape_file);

        // Write a manifest tar at EOD. Returns the tape file number via `out_tape_file`.
        bool write_manifest_at_eod(const std::string &manifest_json,
                                   int &out_tape_file);

        // Rewind to start of tape and write the manifest as tape file 0.
        // Used by mktapir --force to logically reformat a tape. On WORM tapes
        // this will be rejected by the drive if any data already exists.
        bool overwrite_from_start(int block_factor,
                                  const std::function<std::string()> &make_manifest);

    private:
        bool position_latest_manifest();
        bool position_data(int tape_file);
        // FSF to tape_file, then MTFSR by block_within_file (0 = no FSR).
        // block_within_file is the physical-block offset of the member's tar header
        // within the tape file, as recorded in Node::block_number at write time.
        bool seek_to(int tape_file, int64_t block_within_file);
        bool fsr(int64_t n); // MTFSR within the current tape file
        bool write_tape_file(int block_factor, const std::function<bool(struct archive *)> &writer);
        bool ctl(short op, int count); // st driver MTIOCTOP wrapper (<sys/mtio.h>)

        std::string dev_;
        int mbf_;
        bool verbose_ = false;
        int current_file_ = -1; // tracked tape file number; -1 = unknown (avoids rewind when possible)
    };

} // namespace tapir

#endif // TAPIR_TAPE_HPP
