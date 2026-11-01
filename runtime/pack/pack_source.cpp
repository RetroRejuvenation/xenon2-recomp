// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
#include "pack/pack_source.h"

#include "import/importer.h"
#include "import/sha1.h"
#include "pack/pack_paths.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

namespace x2::pack {

namespace fs = std::filesystem;

namespace {

std::string unsafe_message(const std::string& rel) { return "unsafe path '" + rel + "' (relative paths inside the pack only)"; }

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

bool within(const fs::path& p, const fs::path& root) {
    auto r = root.begin(), q = p.begin();
    for (; r != root.end(); ++r, ++q)
        if (q == p.end() || *q != *r) return false;
    return true;
}

// ---- byte sources ----------------------------------------------------------------------------------------------------

class MemorySource final : public ByteSource {
public:
    explicit MemorySource(std::vector<uint8_t> b) : b_(std::move(b)) {}
    uint64_t size() const override { return b_.size(); }
    size_t read(uint64_t offset, void* dst, size_t n) override {
        if (offset >= b_.size()) return 0;
        n = size_t(std::min<uint64_t>(n, b_.size() - offset));
        std::memcpy(dst, b_.data() + offset, n);
        return n;
    }

private:
    std::vector<uint8_t> b_;
};

// A window [base, base + length) of a file on disk, read through its own handle.
class FileWindow final : public ByteSource {
public:
    FileWindow(const fs::path& p, uint64_t base, uint64_t length) : in_(p, std::ios::binary), base_(base), len_(length) {}
    bool ok() const { return bool(in_); }
    uint64_t size() const override { return len_; }
    size_t read(uint64_t offset, void* dst, size_t n) override {
        if (offset >= len_ || !in_) return 0;
        n = size_t(std::min<uint64_t>(n, len_ - offset));
        in_.clear();
        in_.seekg(std::streamoff(base_ + offset));
        if (!in_.read(static_cast<char*>(dst), std::streamsize(n))) {
            const size_t got = size_t(in_.gcount());
            in_.clear();
            return got;
        }
        return n;
    }

private:
    std::ifstream in_;
    uint64_t base_, len_;
};

// ---- directory ---------------------------------------------------------------------------------------------------------

class DirSource final : public PackSource {
public:
    DirSource(std::string path, fs::path root) : path_(std::move(path)), root_(std::move(root)) {}
    const std::string& path() const override { return path_; }
    bool is_zip() const override { return false; }
    std::vector<std::string> names() const override {
        std::vector<std::string> out;
        std::error_code ec;
        fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end && out.size() < 1000000; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            out.push_back(fs::relative(it->path(), root_, e2).generic_string());
        }
        std::sort(out.begin(), out.end());
        return out;
    }
    bool file_size(const std::string& rel, uint64_t& size) const override {
        fs::path p;
        std::string why;
        if (!resolve(rel, p, why)) return false;
        std::error_code ec;
        size = fs::file_size(p, ec);
        return !ec;
    }
    bool streamable(const std::string&) const override { return true; }

protected:
    bool do_read(const std::string& rel, uint64_t limit, std::vector<uint8_t>& out, std::string& why) const override {
        fs::path p;
        if (!resolve(rel, p, why)) return false;
        std::error_code ec;
        const uintmax_t size = fs::file_size(p, ec);
        if (ec) { why = "'" + rel + "': cannot read"; return false; }
        if (size > limit) { why = "'" + rel + "': too large (" + std::to_string(size) + " bytes, limit " + std::to_string(limit) + ")"; return false; }
        std::ifstream in(p, std::ios::binary);
        out.resize(size_t(size));
        if (!in || (size && !in.read(reinterpret_cast<char*>(out.data()), std::streamsize(size)))) {
            why = "'" + rel + "': cannot read";
            return false;
        }
        return true;
    }
    std::unique_ptr<ByteSource> do_open(const std::string& rel, uint64_t limit, std::string& why) const override {
        fs::path p;
        if (!resolve(rel, p, why)) return nullptr;
        std::error_code ec;
        const uintmax_t size = fs::file_size(p, ec);
        if (ec) { why = "'" + rel + "': cannot read"; return nullptr; }
        if (size > limit) { why = "'" + rel + "': too large (" + std::to_string(size) + " bytes, limit " + std::to_string(limit) + ")"; return nullptr; }
        auto w = std::make_unique<FileWindow>(p, 0, size);
        if (!w->ok()) { why = "'" + rel + "': cannot read"; return nullptr; }
        return w;
    }

private:
    bool resolve(const std::string& rel, fs::path& out, std::string& why) const {
        std::error_code e;
        out = fs::weakly_canonical(root_ / fs::path(rel), e);
        if (e || !within(out, root_)) { why = "'" + rel + "' leaves the pack directory"; return false; }
        if (!fs::is_regular_file(out, e)) { why = "missing file '" + rel + "'"; return false; }
        return true;
    }
    std::string path_;
    fs::path root_;
};

// ---- ZIP ----------------------------------------------------------------------------------------------------------------

class ZipSource final : public PackSource {
public:
    struct Member {
        uint16_t flags = 0, method = 0;
        uint32_t crc = 0, csize = 0, usize = 0, lho = 0;
        std::string problem;
    };

    ZipSource(std::string path, const PackLimits& lim) : path_(std::move(path)), lim_(lim) {}

    bool init(std::string& why) {
        std::error_code ec;
        const uintmax_t size = fs::file_size(fs::path(path_), ec);
        if (ec) { why = "cannot read"; return false; }
        if (size > lim_.max_zip) { why = "too large (" + std::to_string(size) + " bytes, limit " + std::to_string(lim_.max_zip) + ")"; return false; }
        in_.open(fs::path(path_), std::ios::binary);
        if (!in_) { why = "cannot read"; return false; }
        size_ = uint64_t(size);
        if (size_ < 22) { why = "truncated zip"; return false; }
        // End of central directory: in the last 22 + 65535 bytes.
        const uint64_t tail = std::min<uint64_t>(size_, 22 + 65535);
        std::vector<uint8_t> t(static_cast<size_t>(tail));
        if (!read_at(size_ - tail, t.data(), t.size())) { why = "cannot read"; return false; }
        size_t e = std::string::npos;
        for (size_t p = t.size() - 22 + 1; p-- > 0;)
            if (le32(&t[p]) == 0x06054b50) { e = p; break; }
        if (e == std::string::npos) { why = "no zip central directory (truncated or corrupted)"; return false; }
        const uint16_t disk = le16(&t[e + 4]), cd_disk = le16(&t[e + 6]), here = le16(&t[e + 8]), count = le16(&t[e + 10]);
        const uint32_t cd_size = le32(&t[e + 12]), cd_off = le32(&t[e + 16]);
        if (count == 0xFFFF || cd_off == 0xFFFFFFFFu || cd_size == 0xFFFFFFFFu) { why = "zip64 archives are not supported"; return false; }
        if (disk != 0 || cd_disk != 0 || here != count) { why = "multi-disk zip archives are not supported"; return false; }
        if (count > lim_.max_zip_members) { why = "zip archive with more than " + std::to_string(lim_.max_zip_members) + " members"; return false; }
        const uint64_t eocd_at = size_ - tail + e;
        if (uint64_t(cd_off) + cd_size > eocd_at || cd_size > (64u << 20)) { why = "corrupted zip central directory"; return false; }
        std::vector<uint8_t> cd(static_cast<size_t>(cd_size));
        if (cd_size && !read_at(cd_off, cd.data(), cd.size())) { why = "cannot read"; return false; }

        struct Raw {
            std::string name;
            Member m;
        };
        std::vector<Raw> raws;
        size_t p = 0;
        bool backslashes = false;
        for (uint32_t i = 0; i < count; ++i) {
            if (cd.size() - p < 46 || le32(&cd[p]) != 0x02014b50) { why = "corrupted zip central directory"; return false; }
            Raw r;
            r.m.flags = le16(&cd[p + 8]);
            r.m.method = le16(&cd[p + 10]);
            r.m.crc = le32(&cd[p + 16]);
            r.m.csize = le32(&cd[p + 20]);
            r.m.usize = le32(&cd[p + 24]);
            r.m.lho = le32(&cd[p + 42]);
            const size_t nlen = le16(&cd[p + 28]), xlen = le16(&cd[p + 30]), clen = le16(&cd[p + 32]);
            if (nlen + xlen + clen > cd.size() - p - 46) { why = "corrupted zip central directory"; return false; }
            bool changed = false;
            r.name = normalise_member_name(std::string(reinterpret_cast<const char*>(&cd[p + 46]), nlen), changed);
            backslashes = backslashes || changed;
            p += 46 + nlen + xlen + clen;
            if (r.name.empty() || r.name.back() == '/' || r.name.rfind("__MACOSX/", 0) == 0) continue; // folders, macOS metadata
            raws.push_back(std::move(r));
        }
        if (backslashes) notes_.push_back("ZIP member names with backslashes were read as '/' (an archive made by an old Windows tool)");

        // manifest.json at the root, or in a single top-level folder.
        bool root_manifest = false;
        std::vector<std::string> nested;
        for (const Raw& r : raws) {
            if (r.name == "manifest.json") root_manifest = true;
            const auto slash = r.name.find('/');
            if (slash != std::string::npos && r.name.compare(slash, std::string::npos, "/manifest.json") == 0) nested.push_back(r.name.substr(0, slash + 1));
        }
        std::string prefix;
        if (!root_manifest) {
            if (nested.size() != 1) { why = "no manifest.json in the ZIP file"; return false; }
            prefix = nested[0];
        }
        std::map<std::string, std::string> folded; // folded name -> the name that claimed it
        for (Raw& r : raws) {
            if (r.name.compare(0, prefix.size(), prefix) != 0) continue; // outside the pack folder
            const std::string rel = r.name.substr(prefix.size());
            const std::string bad = path_problem(rel);
            if (!bad.empty()) {
                notes_.push_back("ZIP member '" + r.name + "' is ignored: " + bad);
                continue;
            }
            const auto [it, fresh] = folded.emplace(fold_path(rel), rel);
            if (!fresh) {
                why = "two members collide on a case-insensitive file system ('" + it->second + "' and '" + rel + "')";
                return false;
            }
            if (r.m.flags & 1) { r.m.problem = "encrypted zip entries are not supported"; }
            else if (r.m.method != 0 && r.m.method != 8) r.m.problem = "zip compression method " + std::to_string(r.m.method) + " is not supported (only stored/deflate)";
            else if (r.m.usize > lim_.max_member) r.m.problem = "too large (" + std::to_string(r.m.usize) + " bytes)";
            members_.emplace(rel, r.m);
        }
        return true;
    }

    const std::string& path() const override { return path_; }
    bool is_zip() const override { return true; }
    std::vector<std::string> names() const override {
        std::vector<std::string> out;
        for (const auto& [n, m] : members_) out.push_back(n);
        return out;
    }
    bool file_size(const std::string& rel, uint64_t& size) const override {
        auto it = members_.find(rel);
        if (it == members_.end()) return false;
        size = it->second.usize;
        return true;
    }
    bool streamable(const std::string& rel) const override {
        auto it = members_.find(rel);
        return it != members_.end() && it->second.method == 0;
    }

protected:
    bool do_read(const std::string& rel, uint64_t limit, std::vector<uint8_t>& out, std::string& why) const override {
        auto it = members_.find(rel);
        if (it == members_.end()) { why = "missing file '" + rel + "'"; return false; }
        const Member& m = it->second;
        if (!m.problem.empty()) { why = "'" + rel + "': " + m.problem; return false; }
        if (m.usize > limit) { why = "'" + rel + "': too large (" + std::to_string(m.usize) + " bytes, limit " + std::to_string(limit) + ")"; return false; }
        uint64_t data = 0;
        if (!locate(m, data, why)) { why = "'" + rel + "': " + why; return false; }
        if (m.method == 0) {
            if (m.csize != m.usize) { why = "'" + rel + "': corrupted (stored size mismatch)"; return false; }
            out.resize(m.usize);
            if (m.usize && !read_at_locked(data, out.data(), out.size())) { why = "'" + rel + "': cannot read"; return false; }
        } else {
            if (m.usize > lim_.ratio_free_bytes && m.usize / std::max<uint32_t>(m.csize, 1) > lim_.max_ratio) {
                why = "'" + rel + "': refused, compression ratio above " + std::to_string(lim_.max_ratio) + ":1";
                return false;
            }
            if (m.csize > m.usize + (m.usize >> 3) + 1024) { why = "'" + rel + "': corrupted deflate data"; return false; }
            std::vector<uint8_t> packed(m.csize);
            if (m.csize && !read_at_locked(data, packed.data(), packed.size())) { why = "'" + rel + "': cannot read"; return false; }
            if (!x2::import::inflate_raw(packed.data(), packed.size(), m.usize, out)) { why = "'" + rel + "': corrupted deflate data"; return false; }
        }
        if (x2::import::crc32(out.data(), out.size()) != m.crc) {
            std::vector<uint8_t>().swap(out);
            why = "'" + rel + "': CRC mismatch (corrupted)";
            return false;
        }
        return true;
    }
    std::unique_ptr<ByteSource> do_open(const std::string& rel, uint64_t limit, std::string& why) const override {
        auto it = members_.find(rel);
        if (it == members_.end()) { why = "missing file '" + rel + "'"; return nullptr; }
        const Member& m = it->second;
        if (m.method == 0 && m.problem.empty() && m.csize == m.usize && m.usize <= limit) {
            uint64_t data = 0;
            if (!locate(m, data, why)) { why = "'" + rel + "': " + why; return nullptr; }
            auto w = std::make_unique<FileWindow>(fs::path(path_), data, m.usize);
            if (!w->ok()) { why = "'" + rel + "': cannot read"; return nullptr; }
            return w;
        }
        std::vector<uint8_t> bytes;
        if (!do_read(rel, limit, bytes, why)) return nullptr;
        return std::make_unique<MemorySource>(std::move(bytes));
    }

private:
    bool read_at(uint64_t off, void* dst, size_t n) const {
        in_.clear();
        in_.seekg(std::streamoff(off));
        return bool(in_.read(static_cast<char*>(dst), std::streamsize(n)));
    }
    bool read_at_locked(uint64_t off, void* dst, size_t n) const {
        std::lock_guard<std::mutex> lock(mu_);
        return read_at(off, dst, n);
    }
    // The offset of a member's data, from its local header.
    bool locate(const Member& m, uint64_t& data, std::string& why) const {
        uint8_t h[30];
        if (uint64_t(m.lho) + 30 > size_ || !read_at_locked(m.lho, h, 30) || le32(h) != 0x04034b50) { why = "corrupted local header"; return false; }
        data = uint64_t(m.lho) + 30 + le16(h + 26) + le16(h + 28);
        if (data > size_ || m.csize > size_ - data) { why = "truncated data"; return false; }
        return true;
    }

    std::string path_;
    PackLimits lim_;
    mutable std::ifstream in_;
    mutable std::mutex mu_;
    uint64_t size_ = 0;
    std::map<std::string, Member> members_;
};

} // namespace

bool PackSource::exists(const std::string& rel) const {
    uint64_t s = 0;
    return safe_pack_path(rel) && file_size(rel, s);
}

bool PackSource::read(const std::string& rel, uint64_t limit, std::vector<uint8_t>& out, std::string& why) const {
    if (!safe_pack_path(rel)) { why = unsafe_message(rel); return false; }
    return do_read(rel, limit, out, why);
}

std::unique_ptr<ByteSource> PackSource::open(const std::string& rel, uint64_t limit, std::string& why) const {
    if (!safe_pack_path(rel)) { why = unsafe_message(rel); return nullptr; }
    return do_open(rel, limit, why);
}

std::unique_ptr<ByteSource> memory_byte_source(std::vector<uint8_t> bytes) { return std::make_unique<MemorySource>(std::move(bytes)); }

std::unique_ptr<PackSource> open_pack_source(const std::string& path, const PackLimits& limits, std::string& why) {
    std::error_code ec;
    if (fs::is_directory(fs::path(path), ec)) {
        const fs::path root = fs::canonical(fs::path(path), ec);
        if (ec) { why = "cannot open the directory"; return nullptr; }
        return std::make_unique<DirSource>(path, root);
    }
    if (fs::is_regular_file(fs::path(path), ec)) {
        auto z = std::make_unique<ZipSource>(path, limits);
        std::string zwhy;
        if (!z->init(zwhy)) {
            why = zwhy == "no manifest.json in the ZIP file" || zwhy.rfind("two members collide", 0) == 0 ? zwhy : "not a usable ZIP file: " + zwhy;
            return nullptr;
        }
        return z;
    }
    why = "no such asset pack (a directory or a .zip file)";
    return nullptr;
}

} // namespace x2::pack
