// manifest.hpp — model of ltfs_to_tar.py's manifest.json + directory tree.
//
// The manifest is a cumulative "array of arrays": one inner array per archive on
// the tape, element 0 a header object, the rest per-file records
// {path, size, hashes, verified_with}. See ltfs_to_tar.py for the writer.

#ifndef TAPIR_MANIFEST_HPP
#define TAPIR_MANIFEST_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tapir {

struct FileEntry {
    std::string path;            // e.g. "2013/Some Video [id].mp4"
    uint64_t    size = 0;
    std::string sha256;          // hashes["sha256sum"], empty if absent
    int         archive_index = 0;
};

struct ArchiveInfo {
    int         index = 0;
    int         data_tape_file = 0;
    int         manifest_tape_file = 0;
    std::string source;
    std::string created;         // ISO-8601, e.g. "2026-06-15T18:26:15"
    int         block_factor = 0;
    uint64_t    file_count = 0;
    uint64_t    total_bytes = 0;
};

struct Manifest {
    std::vector<ArchiveInfo> archives;
    std::vector<FileEntry>   files;

    // Parse the cumulative array-of-arrays manifest JSON. Throws on malformed input.
    static Manifest parse(const std::string& json_text);
};

// In-memory directory tree built from the flat file list.
struct Node {
    std::string name;
    bool        is_dir = false;
    uint64_t    size = 0;                 // for files
    const FileEntry* entry = nullptr;     // non-null for files
    std::map<std::string, std::unique_ptr<Node>> children;
};

// Build the tree (root dir node) from a manifest. The returned tree borrows
// pointers into `m.files`, so `m` must outlive the tree and must not be modified.
std::unique_ptr<Node> build_tree(const Manifest& m);

// Resolve an absolute FUSE path ("/", "/2013", "/2013/foo.mp4") to a node, or nullptr.
const Node* resolve(const Node* root, const std::string& path);

} // namespace tapir

#endif // TAPIR_MANIFEST_HPP
