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
    };

    struct Node
    {
        std::string name;
        bool is_dir = false;
        uint64_t size = 0;
        std::string sha256;
        int data_tape_file = 0;         // tape file holding this member's data
        int block_factor = 0;           // blocking factor of that tape file
        std::shared_ptr<Staged> staged; // non-null while data lives only in a temp file
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
                      int data_tape_file, int block_factor);

        bool make_dir(const std::string &path);
        bool remove_dir(const std::string &path);
        bool unlink_file(const std::string &path); // index-only delete

        bool dirty() const { return dirty_; }
        void mark_dirty() { dirty_ = true; }

        const Node *root() const { return root_.get(); }
        Node *root() { return root_.get(); }

        // Serialise to manifest.json. Staged files are assigned to a new archive at
        // tape file `new_data_tape_file` with blocking factor `new_block_factor`.
        std::string serialize(int new_data_tape_file, int new_block_factor) const;

        std::vector<FileRec> flat() const; // all current files

        std::string source;
        std::string created;

    private:
        struct Meta
        {
            int manifest_tape_file = 0;
            int block_factor = 0;
            std::string source, created;
        };

        Node *ensure_path(const std::string &path, bool leaf_is_dir);
        Node *parent_of(const std::vector<std::string> &parts);

        std::unique_ptr<Node> root_ = std::make_unique<Node>();
        std::map<int, Meta> meta_; // data_tape_file -> archive header metadata
        bool dirty_ = false;
    };

} // namespace tapir

#endif // TAPIR_INDEX_HPP
