// tape.hpp — tape positioning + tape-file tar I/O on the no-rewind device.
//
// Wraps `mt` for positioning and libarchive (block-aligned) for reading/writing
// whole tape files. Mirrors ltfs_to_tar.py's tape layout: a data archive at tape
// file N, then the cumulative manifest tar at the following file. Part of libtapir.

#ifndef TAPIR_TAPE_HPP
#define TAPIR_TAPE_HPP

#include "raii.hpp"
#include "tar_io.hpp"

#include <functional>
#include <string>
#include <vector>

namespace tapir {

class Tape {
public:
    // `device` is the no-rewind node (…-nst); `manifest_block_factor` is the
    // blocking factor (×512) used for the manifest tape files (the -b option).
    Tape(std::string device, int manifest_block_factor)
        : dev_(std::move(device)), mbf_(manifest_block_factor) {}

    // positioning (over `mt -f <dev> …`)
    bool rewind();
    bool eod();
    bool fsf(int n);
    bool bsf(int n);
    int  file_number();             // current tape file number (mt status), or -1 on error

    // Read manifest.json from the latest manifest (the last tape file).
    bool read_latest_manifest(std::string& out);

    // Extract one member from the data archive at `tape_file` (streams the tar).
    bool read_member(int tape_file, int block_factor, const std::string& member,
                     Fd& out_fd, uint64_t& out_size);

    // Stream every member of the data archive at `tape_file`, calling cb per file.
    bool scan_archive(int tape_file, int block_factor,
                      const std::function<void(const std::string&, const std::string&, uint64_t)>& cb);

    // Append a new data archive at EOD (written by `write_data`, or skipped if it
    // is empty/null), then write the manifest as the next tape file.
    // `make_manifest(data_tape_file)` produces the manifest JSON, so the new
    // archive's records can reference the tape file it landed on.
    // `out_data_tape_file` receives that tape file number.
    bool append(int block_factor,
                const std::function<bool(struct archive*)>& write_data,
                const std::function<std::string(int data_tape_file)>& make_manifest,
                int& out_data_tape_file);

private:
    bool position_latest_manifest();
    bool position_data(int tape_file);
    bool write_tape_file(int block_factor, const std::function<bool(struct archive*)>& writer);
    bool ctl(short op, int count);   // st driver MTIOCTOP wrapper (<sys/mtio.h>)

    std::string dev_;
    int         mbf_;
};

} // namespace tapir

#endif // TAPIR_TAPE_HPP
