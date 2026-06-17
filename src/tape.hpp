// tape.hpp — tape positioning + tape-file tar I/O on the no-rewind device.
//
// Uses the Linux st-driver ioctls for positioning and libarchive (block-aligned)
// for reading/writing whole tape files. Mirrors ltfs_to_tar.py's tape layout: a
// data archive at tape file N, then the cumulative manifest tar at the following
// file. Part of libtapir.

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

        // Positioning
        bool rewind();
        bool eod();
        bool fsf(int n);
        bool bsf(int n);
        int file_number(); // current tape file number (mt status), or -1 on error

        // End-of-data survey in one pass: returns the number of tape files (or -1
        // on error) and sets `full` if the tape is at end-of-tape — i.e. there is
        // no room to append an index (e.g. a tar that spans onto another tape).
        int survey(bool &full);

        // Read manifest.json from the latest manifest (the last tape file). Fast:
        // only the first member is inspected, so a trailing *data* tape file is
        // rejected without scanning it.
        bool read_latest_manifest(std::string &out);

        // Extract one member from the data archive at `tape_file` (streams the tar).
        bool read_member(int tape_file, int block_factor, const std::string &member,
                         Fd &out_fd, uint64_t &out_size);

        // Stream every member of the data archive at `tape_file`, calling cb per file.
        bool scan_archive(int tape_file, int block_factor,
                          const std::function<void(const std::string &, const std::string &, uint64_t)> &cb);

        // Like scan_archive, but auto-detects the data archive's physical block size
        // (reported via `detected_block_bytes`) by reading with an oversized buffer —
        // a single pass that both scans and discovers the block factor.
        bool scan_archive_detect(int tape_file, int &detected_block_bytes,
                                 const std::function<void(const std::string &, const std::string &, uint64_t)> &cb);

        // Append a new data archive at EOD (written by `write_data`, or skipped if it
        // is empty/null), then write the manifest as the next tape file.
        // `make_manifest(data_tape_file)` produces the manifest JSON, so the new
        // archive's records can reference the tape file it landed on.
        // `out_data_tape_file` receives that tape file number.
        bool append(int block_factor,
                    const std::function<bool(struct archive *)> &write_data,
                    const std::function<std::string(int data_tape_file)> &make_manifest,
                    int &out_data_tape_file);

    private:
        bool position_latest_manifest();
        bool position_data(int tape_file);
        bool write_tape_file(int block_factor, const std::function<bool(struct archive *)> &writer);
        bool ctl(short op, int count); // st driver MTIOCTOP wrapper (<sys/mtio.h>)

        std::string dev_;
        int mbf_;
    };

} // namespace tapir

#endif // TAPIR_TAPE_HPP
