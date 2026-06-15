#include "manifest.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <stdexcept>
#include <utility>

namespace tapir {

using json = nlohmann::json;

Manifest Manifest::parse(const std::string& json_text) {
    json root = json::parse(json_text);   // throws nlohmann::json::parse_error on bad input
    if (!root.is_array())
        throw std::runtime_error("manifest: top level is not an array");

    Manifest m;
    int ai = 0;
    for (const auto& arc : root) {
        if (!arc.is_array() || arc.empty())
            throw std::runtime_error("manifest: archive entry is not a non-empty array");

        const json& h = arc[0];
        ArchiveInfo info;
        info.index              = h.at("index").get<int>();
        info.data_tape_file     = h.at("data_tape_file").get<int>();
        info.manifest_tape_file = h.at("manifest_tape_file").get<int>();
        info.source             = h.value("source", std::string{});
        info.created            = h.value("created", std::string{});
        info.block_factor       = h.value("block_factor", 0);
        info.file_count         = h.value("file_count", static_cast<uint64_t>(0));
        info.total_bytes        = h.value("total_bytes", static_cast<uint64_t>(0));
        m.archives.push_back(std::move(info));

        for (std::size_t i = 1; i < arc.size(); ++i) {
            const json& f = arc[i];
            FileEntry e;
            e.path = f.at("path").get<std::string>();
            e.size = f.at("size").get<uint64_t>();
            e.archive_index = ai;
            if (auto it = f.find("hashes"); it != f.end() && it->is_object()) {
                if (auto hit = it->find("sha256sum"); hit != it->end())
                    e.sha256 = hit->get<std::string>();
            }
            m.files.push_back(std::move(e));
        }
        ++ai;
    }
    return m;
}

static std::vector<std::string> split_path(const std::string& p) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : p) {
        if (c == '/') {
            if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

std::unique_ptr<Node> build_tree(const Manifest& m) {
    auto root = std::make_unique<Node>();
    root->is_dir = true;

    for (const auto& fe : m.files) {
        auto parts = split_path(fe.path);
        if (parts.empty()) continue;
        Node* cur = root.get();
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const bool last = (i + 1 == parts.size());
            auto it = cur->children.find(parts[i]);
            if (it == cur->children.end()) {
                auto n = std::make_unique<Node>();
                n->name = parts[i];
                if (last) {
                    n->is_dir = false;
                    n->size = fe.size;
                    n->entry = &fe;
                } else {
                    n->is_dir = true;
                }
                Node* raw = n.get();
                cur->children.emplace(parts[i], std::move(n));
                cur = raw;
            } else {
                cur = it->second.get();
            }
        }
    }
    return root;
}

const Node* resolve(const Node* root, const std::string& path) {
    if (path.empty() || path == "/") return root;
    const Node* cur = root;
    for (const auto& part : split_path(path)) {
        if (!cur->is_dir) return nullptr;
        auto it = cur->children.find(part);
        if (it == cur->children.end()) return nullptr;
        cur = it->second.get();
    }
    return cur;
}

} // namespace tapir
