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

    // Random v4 UUID (8-4-4-4-12 hex) from a CSPRNG — uniquely identifies a tape.
    static std::string generate_uuid_v4()
    {
        const auto rnd = security::CsprngBytes<16>();
        unsigned char b[16];
        std::memcpy(b, rnd.data(), sizeof b);
        b[6] = (b[6] & 0x0F) | 0x40; // version 4
        b[8] = (b[8] & 0x3F) | 0x80; // variant 1
        char s[37];
        std::snprintf(s, sizeof s,
                      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                      b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                      b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        return std::string(s);
    }

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
            dirty_ = true;
        return n;
    }

    void Index::add_file(const std::string &path, uint64_t size, const std::string &sha256,
                         int dtf, int bf, time_t mtime)
    {
        Node *n = ensure_path(path, false);
        if (!n)
            return; // already indexed — keep the existing entry
        n->size = size;
        n->sha256 = sha256;
        n->mtime = mtime;
        n->data_tape_file = dtf;
        n->block_factor = bf;
        if (meta_.find(dtf) == meta_.end())
        { // register this archive's header metadata
            Meta m;
            m.manifest_tape_file = dtf + 1;
            m.block_factor = bf;
            m.source = source;
            m.created = created;
            meta_[dtf] = m;
        }
        dirty_ = true;
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
        dirty_ = true;
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
        dirty_ = true;
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
            m.generation = h.value("write_generation", static_cast<uint64_t>(0));
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
            out.push_back({path, n->size, n->sha256, n->data_tape_file, n->block_factor});
        return out;
    }

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
            volume_uuid_ = generate_uuid_v4(); // first tapir write to this tape

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

} // namespace tapir
