// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

// index.hpp — the mutable in-memory index (directory tree) for a tape archive.
//
// Single source of truth: nodes own their children via unique_ptr; serialising
// the tree reproduces the cumulative manifest. Each file knows which tape file
// holds its data (data_tape_file) and at what blocking factor. Deletes are
// index-only; new files are staged in a temp file and committed to a new tape
// file at unmount. Part of libtapir.

#ifndef TAPIR_INDEX_HPP
#define TAPIR_INDEX_HPP

#include "raii.hpp"

#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tapir
{

    struct WriteHandle; // FUSE-side write state (defined in tapir.cpp)

    // Bytes of a session-appended file, held until flushed to a new tape file.
    struct Staged
    {
        Fd fd;
        uint64_t size = 0;
        time_t mtime = 0; // user-visible mtime; written into the tar header at unmount
    };

    struct Node
    {
        std::string name;
        bool is_dir = false;
        uint64_t size = 0;
        std::string sha256;
        time_t mtime = 0; // per-file mtime; 0 means use the filesystem mount time
        int data_tape_file = 0;         // tape file holding this member's data
        int block_factor = 0;           // blocking factor of that tape file
        int64_t block_number = -1;      // header's physical block within its tape file; -1 = unknown
        int64_t block_offset = -1;      // header's byte offset within that block; -1 = unknown (slow read)
        mode_t mode = 0;                // permission bits from tar header; 0 = not recorded (use kFileMode default)
        std::string tape_name;          // actual tar member name if different from logical path (set on rename)
        std::shared_ptr<Staged> staged; // non-null while data lives only in a temp file
        bool staged_flushed = false;    // staged data is on the (still-open) tape file; release at sync
        WriteHandle *writing = nullptr; // non-owning observer while open for writing
        std::map<std::string, std::unique_ptr<Node>> children;
    };

    // Flat per-file view (for tfsck).
    struct FileRec
    {
        std::string path;
        uint64_t size = 0;
        std::string sha256;
        int data_tape_file = 0;
        int block_factor = 0;
        int64_t block_number = -1; // -1 = not recorded (old manifest without tape_block)
        int64_t block_offset = -1; // -1 = not recorded (header's byte offset within its block)
    };

    class Index
    {
    public:
        Index() { root_->is_dir = true; }

        void load(const std::string &manifest_json); // throws on malformed input

        const Node *resolve(const std::string &path) const;
        Node *resolve(const std::string &path);

        Node *create_file(const std::string &path); // nullptr if exists / parent missing

        // Import an existing member (e.g. when converting a pre-existing tar tape file):
        // records the file at the given tape file + block factor. No-op if it already exists.
        void add_file(const std::string &path, uint64_t size, const std::string &sha256,
                      int data_tape_file, int block_factor, time_t mtime = 0, mode_t mode = 0);

        // Fill in the per-member block location (block_number + block_offset) for the node
        // at `name` if it belongs to `tape_file` and the location is not yet recorded.
        // Returns true if the node was updated. Shared by mktapir import and tfsck verify.
        bool fill_block_location(const std::string &name, int tape_file,
                                 int64_t block, int64_t offset);

        bool make_dir(const std::string &path);
        bool remove_dir(const std::string &path);
        bool unlink_file(const std::string &path); // index-only delete

        // Move/rename a node (file or directory). Pure index operation — no tape I/O.
        // Preserves tape_name so renamed files remain readable by position (or by their
        // original member name as a fallback when block_offset is unknown).
        // Returns 0 on success or an errno on failure (ENOENT, ENOTEMPTY, EISDIR, ENOTDIR).
        int rename_node(const std::string &from, const std::string &to);

        bool dirty() const { return dirty_; }
        void mark_dirty() { dirty_ = true; ++version_; }
        void mark_clean() { dirty_ = false; } // intentionally leaves version_ alone

        // Monotonic mutation counter, bumped by every index-changing operation (via
        // mark_dirty) but NOT by serialize() or mark_clean(). The writer snapshots
        // it under the lock right after serialize(), then — once the manifest is on
        // tape — only marks the index clean if version() is unchanged, so a FUSE
        // mutation that raced the (unlocked) tape write is not silently dropped.
        uint64_t version() const { return version_; }

        // Drop the staged temp copy of every file whose data is already on a now-closed
        // tape file (staged_flushed). Called by the writer at sync, once the tape file
        // is closed and the members are re-readable from tape.
        void release_flushed_staged();

        const Node *root() const { return root_.get(); }
        Node *root() { return root_.get(); }

        // Serialise to manifest.json. Staged files are assigned to a new archive at
        // tape file `new_data_tape_file` with blocking factor `new_block_factor`.
        // `manifest_tape_file` is the tape file this manifest will occupy; it fills in
        // each archive's `manifest_tape_file` the first time that archive is written
        // (and is preserved on later cumulative manifests, so every archive keeps the
        // location of the manifest from its own generation — what the future snapshot
        // view needs). Pass -1 to fall back to the data-file+1 layout used by the FUSE
        // writer, whose single data file is always immediately followed by its manifest.
        // Generates the volume UUID on first write and stamps the new archive with
        // the next write-generation (so it is non-const).
        std::string serialize(int new_data_tape_file, int new_block_factor,
                              int manifest_tape_file = -1);

        std::vector<FileRec> flat() const; // all current files

        // Per-volume identity (see the encryption design in tape.hpp): a random v4
        // UUID generated once per tape, and a monotonic write-generation counter.
        const std::string &volume_uuid() const { return volume_uuid_; }
        uint64_t latest_generation() const;

        // The manifest directory: every manifest ever written on this tape, as
        // (write_generation, manifest_tape_file) pairs sorted by tape file. Each
        // manifest records its own location, so loading one manifest yields the
        // location of them all — what the future .tapir/ snapshot view seeks with.
        std::vector<std::pair<uint64_t, int>> generations() const;

        std::string source;
        std::string created;

    private:
        struct Meta
        {
            int block_factor = 0;
            uint64_t generation = 0; // write-generation this archive was written in
            std::string source, created;
        };

        Node *ensure_path(const std::string &path, bool leaf_is_dir);
        Node *parent_of(const std::vector<std::string> &parts);

        std::unique_ptr<Node> root_ = std::make_unique<Node>();
        std::map<int, Meta> meta_;       // data_tape_file -> archive header metadata
        std::map<int, uint64_t> manifests_; // manifest_tape_file -> write_generation (the directory)
        std::string volume_uuid_;  // random v4 UUID, constant per tape; generated on first write
        bool dirty_ = false;
        uint64_t version_ = 0;     // bumped by mark_dirty(); see version()
    };

} // namespace tapir

#endif // TAPIR_INDEX_HPP
