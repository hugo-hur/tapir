// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

#include "index.hpp"
#include "security.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <utility>

namespace tapir
{

    using json = nlohmann::json;

    static std::vector<std::string> split(const std::string &p)
    {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : p)
        {
            if (c == '/')
            {
                if (!cur.empty() && cur != ".") // skip "" and "." (e.g. a "./" prefix)
                    parts.push_back(cur);
                cur.clear();
            }
            else
                cur += c;
        }
        if (!cur.empty() && cur != ".")
            parts.push_back(cur);
        return parts;
    }

    // ── lookup ────────────────────────────────────────────────────────────────────
    Node *Index::resolve(const std::string &path)
    {
        if (path.empty() || path == "/")
            return root_.get();
        Node *cur = root_.get();
        for (const auto &part : split(path))
        {
            if (!cur->is_dir)
                return nullptr;
            auto it = cur->children.find(part);
            if (it == cur->children.end())
                return nullptr;
            cur = it->second.get();
        }
        return cur;
    }
    const Node *Index::resolve(const std::string &path) const
    {
        return const_cast<Index *>(this)->resolve(path);
    }

    Node *Index::parent_of(const std::vector<std::string> &parts)
    {
        Node *cur = root_.get();
        for (std::size_t i = 0; i + 1 < parts.size(); ++i)
        {
            auto it = cur->children.find(parts[i]);
            if (it == cur->children.end() || !it->second->is_dir)
                return nullptr;
            cur = it->second.get();
        }
        return cur;
    }

    // ── mutations ─────────────────────────────────────────────────────────────────
    Node *Index::ensure_path(const std::string &path, bool leaf_is_dir)
    {
        auto parts = split(path);
        if (parts.empty())
            return nullptr;
        Node *cur = root_.get();
        for (std::size_t i = 0; i < parts.size(); ++i)
        {
            const bool last = (i + 1 == parts.size());
            auto it = cur->children.find(parts[i]);
            if (it == cur->children.end())
            {
                auto n = std::make_unique<Node>();
                n->name = parts[i];
                n->is_dir = last ? leaf_is_dir : true;
                Node *raw = n.get();
                cur->children.emplace(parts[i], std::move(n));
                cur = raw;
            }
            else
            {
                if (last)
                    return nullptr;
                if (!it->second->is_dir)
                    return nullptr;
                cur = it->second.get();
            }
        }
        return cur;
    }

    Node *Index::create_file(const std::string &path)
    {
        Node *n = ensure_path(path, false);
        if (n)
            mark_dirty();
        return n;
    }

    void Index::add_file(const std::string &path, uint64_t size, const std::string &sha256,
                         int dtf, int bf, time_t mtime, mode_t mode)
    {
        // Resolve existing node first so the last occurrence on tape wins,
        // matching normal tar extract/append semantics.
        Node *n = resolve(path);
        if (n)
        {
            if (n->is_dir || n->staged)
                return; // directory or active FUSE write — leave untouched
        }
        else
        {
            n = ensure_path(path, false);
            if (!n)
                return; // parent path missing or structural conflict
        }
        n->size = size;
        n->sha256 = sha256;
        n->mtime = mtime;
        n->data_tape_file = dtf;
        n->block_factor = bf;
        n->block_number = -1; // tape location changed; filled in by tfsck verify if needed
        n->block_offset = -1;
        n->mode = mode;
        if (meta_.find(dtf) == meta_.end())
        { // register this archive's header metadata
            Meta m;
            m.manifest_tape_file = dtf + 1;
            m.block_factor = bf;
            m.source = source;
            m.created = created;
            meta_[dtf] = m;
        }
        mark_dirty();
    }
    bool Index::fill_block_location(const std::string &name, int tape_file,
                                   int64_t block, int64_t offset)
    {
        Node *n = resolve(name);
        if (n && n->data_tape_file == tape_file &&
            (n->block_number < 0 || n->block_offset < 0))
        {
            n->block_number = block;
            n->block_offset = offset;
            return true;
        }
        return false;
    }

    bool Index::make_dir(const std::string &path)
    {
        return ensure_path(path, true) != nullptr;
    }
    bool Index::remove_dir(const std::string &path)
    {
        auto parts = split(path);
        if (parts.empty())
            return false;
        Node *parent = parent_of(parts);
        if (!parent)
            return false;
        auto it = parent->children.find(parts.back());
        if (it == parent->children.end() || !it->second->is_dir || !it->second->children.empty())
            return false;
        parent->children.erase(it);
        mark_dirty();
        return true;
    }
    bool Index::unlink_file(const std::string &path)
    {
        auto parts = split(path);
        if (parts.empty())
            return false;
        Node *parent = parent_of(parts);
        if (!parent)
            return false;
        auto it = parent->children.find(parts.back());
        if (it == parent->children.end() || it->second->is_dir)
            return false;
        parent->children.erase(it); // index-only: archived data is left untouched
        mark_dirty();
        return true;
    }

    // ── load ──────────────────────────────────────────────────────────────────────
    void Index::load(const std::string &text)
    {
        json root = json::parse(text);
        if (!root.is_array())
            throw std::runtime_error("manifest: top level is not an array");

        bool first = true;
        for (const auto &arc : root)
        {
            if (!arc.is_array() || arc.empty())
                throw std::runtime_error("manifest: archive entry is not a non-empty array");
            const json &h = arc[0];
            const int dtf = h.value("data_tape_file", 0);
            const int bf = h.value("block_factor", 0);
            Meta m;
            m.manifest_tape_file = h.value("manifest_tape_file", dtf + 1);
            m.block_factor = bf;
            m.generation = h.value("write_generation", 0ULL);
            m.source = h.value("source", std::string{});
            m.created = h.value("created", std::string{});
            meta_[dtf] = m;
            if (const std::string vu = h.value("volume_uuid", std::string{}); volume_uuid_.empty() && !vu.empty())
                volume_uuid_ = vu; // volume id is constant across the tape
            if (first)
            {
                source = m.source;
                created = m.created;
                first = false;
            }

            for (std::size_t i = 1; i < arc.size(); ++i)
            {
                const json &f = arc[i];
                Node *n = ensure_path(f.at("path").get<std::string>(), false);
                if (!n)
                    continue;
                n->size = f.at("size").get<uint64_t>();
                n->mtime = f.value("mtime", static_cast<time_t>(0));
                n->data_tape_file = dtf;
                n->block_factor = bf;
                n->block_number = f.value("tape_block", -1LL);
                n->block_offset = f.value("tape_block_offset", -1LL);
                n->mode = static_cast<mode_t>(f.value("perm", 0));
                n->tape_name = f.value("tape_member", std::string{});
                if (auto it = f.find("hashes"); it != f.end() && it->is_object())
                    if (auto hit = it->find("sha256sum"); hit != it->end())
                        n->sha256 = hit->get<std::string>();
            }
        }
        dirty_ = false;
    }

    // ── collect / serialise ───────────────────────────────────────────────────────
    static void collect(const Node *n, const std::string &prefix,
                        std::vector<std::pair<std::string, const Node *>> &out)
    {
        for (const auto &[name, child] : n->children)
        {
            const std::string p = prefix.empty() ? name : prefix + "/" + name;
            if (child->is_dir)
                collect(child.get(), p, out);
            else
                out.emplace_back(p, child.get());
        }
    }

    std::vector<FileRec> Index::flat() const
    {
        std::vector<std::pair<std::string, const Node *>> files;
        collect(root_.get(), "", files);
        std::vector<FileRec> out;
        out.reserve(files.size());
        for (const auto &[path, n] : files)
            out.push_back({path, n->size, n->sha256, n->data_tape_file, n->block_factor, n->block_number, n->block_offset});
        return out;
    }

    static void release_flushed_walk(Node *n)
    {
        for (auto &[name, child] : n->children)
        {
            if (child->is_dir)
                release_flushed_walk(child.get());
            else if (child->staged && child->staged_flushed)
            {
                child->staged.reset(); // member is now on a closed tape file; free the temp
                child->staged_flushed = false;
            }
        }
    }
    void Index::release_flushed_staged() { release_flushed_walk(root_.get()); }

    uint64_t Index::latest_generation() const
    {
        uint64_t g = 0;
        for (const auto &[k, m] : meta_)
            g = std::max(g, m.generation);
        return g;
    }

    std::string Index::serialize(int new_dtf, int new_bf)
    {
        if (volume_uuid_.empty())
            volume_uuid_ = security::UuidV4(); // first tapir write to this tape

        std::vector<std::pair<std::string, const Node *>> files;
        collect(root_.get(), "", files);

        // group by the tape file that holds each member
        std::map<int, std::vector<std::pair<std::string, const Node *>>> groups;
        for (auto &pr : files)
        {
            const int key = pr.second->staged ? new_dtf : pr.second->data_tape_file;
            groups[key].push_back(pr);
        }

        const uint64_t next_gen = latest_generation() + 1; // for archives written this session

        char now[32] = {0};
        std::time_t t = std::time(nullptr);
        std::tm tmv{};
        if (gmtime_r(&t, &tmv))
            std::strftime(now, sizeof now, "%Y-%m-%dT%H:%M:%S", &tmv);

        json out = json::array();
        int idx = 0;
        for (const auto &[dtf, v] : groups)
        {
            auto mit = meta_.find(dtf);
            const bool is_new = (mit == meta_.end());
            const uint64_t gen = is_new ? next_gen : mit->second.generation;
            uint64_t total = 0;
            for (const auto &pr : v)
                total += pr.second->size;

            json arc = json::array();
            json h = json::object();
            h["index"] = idx;
            h["data_tape_file"] = dtf;
            h["manifest_tape_file"] = is_new ? dtf + 1 : mit->second.manifest_tape_file;
            h["volume_uuid"] = volume_uuid_;
            h["write_generation"] = gen;
            h["source"] = is_new ? source : mit->second.source;
            h["created"] = is_new ? std::string(now) : mit->second.created;
            h["block_factor"] = is_new ? new_bf : mit->second.block_factor;
            h["file_count"] = v.size();
            h["total_bytes"] = total;
            arc.push_back(std::move(h));
            for (const auto &[path, node] : v)
            {
                json f = json::object();
                f["path"] = path;
                f["size"] = node->size;
                f["mtime"] = node->mtime;
                f["hashes"] = node->sha256.empty()
                                  ? json::object()
                                  : json::object({{"sha256sum", node->sha256}});
                if (node->block_number >= 0)
                    f["tape_block"] = node->block_number;
                if (node->block_offset >= 0)
                    f["tape_block_offset"] = node->block_offset;
                if (!node->tape_name.empty())
                    f["tape_member"] = node->tape_name;
                if (node->mode != 0)
                    f["perm"] = node->mode;
                f["verified_with"] = nullptr;
                arc.push_back(std::move(f));
            }
            out.push_back(std::move(arc));
            ++idx;

            if (is_new) // record so latest_generation() reflects this write
            {
                Meta m;
                m.manifest_tape_file = dtf + 1;
                m.block_factor = new_bf;
                m.generation = gen;
                m.source = source;
                m.created = now;
                meta_[dtf] = m;
            }
        }
        return out.dump();
    }

    // ── rename ────────────────────────────────────────────────────────────────────
    // Recursively set tape_name for files in a subtree about to be moved.
    // old_path is the pre-move member path (no leading '/').
    static void fix_tape_names(Node *n, const std::string &old_path)
    {
        for (auto &[name, child] : n->children)
        {
            const std::string child_old = old_path + "/" + name;
            if (child->is_dir)
                fix_tape_names(child.get(), child_old);
            else if (child->tape_name.empty())
                child->tape_name = child_old;
        }
    }

    int Index::rename_node(const std::string &from, const std::string &to)
    {
        if (from == to)
            return 0;

        auto from_parts = split(from);
        if (from_parts.empty())
            return EINVAL;
        Node *from_parent = parent_of(from_parts);
        if (!from_parent)
            return ENOENT;
        auto from_it = from_parent->children.find(from_parts.back());
        if (from_it == from_parent->children.end())
            return ENOENT;
        Node *src = from_it->second.get();

        auto to_parts = split(to);
        if (to_parts.empty())
            return EINVAL;
        Node *to_parent = parent_of(to_parts);
        if (!to_parent || !to_parent->is_dir)
            return ENOENT;

        // Handle existing destination
        auto to_it = to_parent->children.find(to_parts.back());
        if (to_it != to_parent->children.end())
        {
            Node *dst = to_it->second.get();
            if (dst->is_dir && !src->is_dir)  return EISDIR;
            if (!dst->is_dir && src->is_dir)   return ENOTDIR;
            if (dst->is_dir && !dst->children.empty()) return ENOTEMPTY;
            to_parent->children.erase(to_it);
        }

        // Preserve the original tar member name before the logical path changes.
        // Member names on tape have no leading '/'; strip it from the FUSE path.
        const std::string old_member = (!from.empty() && from[0] == '/') ? from.substr(1) : from;
        if (!src->is_dir)
        {
            if (src->tape_name.empty())
                src->tape_name = old_member;
        }
        else
        {
            fix_tape_names(src, old_member);
        }

        src->name = to_parts.back();
        to_parent->children.emplace(to_parts.back(), std::move(from_it->second));
        from_parent->children.erase(from_parts.back());
        mark_dirty();
        return 0;
    }

} // namespace tapir
