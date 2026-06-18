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

        // End-of-data survey in one pass: returns the number of tape files (or -1
        // on error) and sets `full` if the tape is at end-of-tape — i.e. there is
        // no room to append an index (e.g. a tar that spans onto another tape).
        int survey(bool &full);

        // Read manifest.json from the latest manifest (the last tape file). Fast:
        // only the first member is inspected, so a trailing *data* tape file is
        // rejected without scanning it.
        bool read_latest_manifest(std::string &out);

        // Read manifest.json from a specific tape file (for generation enumeration
        // and rollback). Returns false if the tape file is not a tapir manifest.
        bool read_manifest_at(int tape_file, std::string &out);

        // Extract one member from the data archive at `tape_file` (streams the tar).
        bool read_member(int tape_file, int block_factor, const std::string &member,
                         Fd &out_fd, uint64_t &out_size);

        // Stream every member of the data archive at `tape_file`, calling cb per file.
        // on_header(name) fires immediately on header read; cb fires after data is hashed.
        bool scan_archive(int tape_file, int block_factor,
                          const std::function<void(const std::string &, const std::string &, uint64_t, time_t)> &cb,
                          const std::function<void(const std::string &)> &on_header = {});

        // Like scan_archive, but auto-detects the data archive's physical block size
        // (reported via `detected_block_bytes`) by reading with an oversized buffer —
        // a single pass that both scans and discovers the block factor.
        bool scan_archive_detect(int tape_file, int &detected_block_bytes,
                                 const std::function<void(const std::string &, const std::string &, uint64_t, time_t)> &cb,
                                 const std::function<void(const std::string &)> &on_header = {});

        // Append a new data archive at EOD (written by `write_data`, or skipped if it
        // is empty/null), then write the manifest as the next tape file.
        // `make_manifest(data_tape_file)` produces the manifest JSON, so the new
        // archive's records can reference the tape file it landed on.
        // `out_data_tape_file` receives that tape file number.
        bool append(int block_factor,
                    const std::function<bool(struct archive *)> &write_data,
                    const std::function<std::string(int data_tape_file)> &make_manifest,
                    int &out_data_tape_file);

        // Primitives used by the background writer thread (tapir). Each call
        // positions to EOD before writing, so they compose safely when serialised
        // through a single-writer queue.

        // Write one file as a single-member tar at EOD. Returns the tape file number
        // of the written archive via `out_tape_file`.
        bool write_data_at_eod(int block_factor,
                               const std::function<bool(struct archive *)> &writer,
                               int &out_tape_file);

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
        bool write_tape_file(int block_factor, const std::function<bool(struct archive *)> &writer);
        bool ctl(short op, int count); // st driver MTIOCTOP wrapper (<sys/mtio.h>)

        std::string dev_;
        int mbf_;
        bool verbose_ = false;
        int current_file_ = -1; // tracked tape file number; -1 = unknown (avoids rewind when possible)
    };

} // namespace tapir

#endif // TAPIR_TAPE_HPP
