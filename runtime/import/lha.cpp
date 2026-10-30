// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the xenon2-recomp authors
// LHA / LZH reader; see lha.h.
#include "import/lha.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace x2::import {

namespace {

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// ---------------------------------------------------------------------------------------------------------
// Bit input, most significant bit first. Reading past the end reads zeros and sets `over`.

struct Bits {
    const uint8_t* p;
    size_t nbytes;
    size_t pos = 0; // in bits
    bool over = false;

    Bits(const uint8_t* data, size_t n) : p(data), nbytes(n) {}
    uint32_t peek(int k) const { // 0 <= k <= 16
        size_t byte = pos >> 3;
        unsigned sh = unsigned(pos & 7);
        uint32_t w = 0;
        for (size_t i = 0; i < 3; ++i) w = (w << 8) | (byte + i < nbytes ? p[byte + i] : 0u);
        return (w >> (24 - sh - unsigned(k))) & ((1u << k) - 1u);
    }
    void skip(int k) {
        pos += size_t(k);
        if (pos > nbytes * 8) over = true;
    }
    uint32_t read(int k) {
        uint32_t v = peek(k);
        skip(k);
        return v;
    }
    int bit() { return int(read(1)); }
};

// Canonical Huffman code as LHA assigns them: codes of one length are consecutive in symbol order, shorter codes
// first. `single` is the special "table of one symbol of length zero" that costs no bits.
struct Huff {
    uint16_t count[17] = {};
    uint16_t sym[512] = {};
    int single = -1;

    bool build(const uint8_t* lens, int n) {
        single = -1;
        std::memset(count, 0, sizeof count);
        for (int i = 0; i < n; ++i) {
            if (lens[i] > 16) return false;
            ++count[lens[i]];
        }
        count[0] = 0;
        int left = 1;
        for (int len = 1; len <= 16; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return false; // over-subscribed
        }
        uint16_t offs[18] = {};
        for (int len = 1; len <= 16; ++len) offs[len + 1] = uint16_t(offs[len] + count[len]);
        for (int i = 0; i < n; ++i)
            if (lens[i]) sym[offs[lens[i]]++] = uint16_t(i);
        return true;
    }
    void set_single(int s) {
        single = s;
        std::memset(count, 0, sizeof count);
    }
    int decode(Bits& b) const {
        if (single >= 0) return single;
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 16; ++len) {
            code |= b.bit();
            int cnt = count[len];
            if (code - first < cnt) return sym[index + code - first];
            index += cnt;
            first += cnt;
            first <<= 1;
            code <<= 1;
            if (b.over) return -1;
        }
        return -1;
    }
};

// ---------------------------------------------------------------------------------------------------------
// -lh4- .. -lh7-: blocks, each with its own code tables (temp table -> literal/length table, offset table).

constexpr int kThreshold = 3;
constexpr int kNumCodes = 510;
constexpr int kMaxTemp = 31;

int read_length_value(Bits& b) {
    int len = int(b.read(3));
    if (len == 7)
        while (b.bit()) {
            if (++len > 16 || b.over) return -1;
        }
    return len;
}

bool decode_new(const uint8_t* src, size_t n, size_t out_len, std::vector<uint8_t>& out, int offset_bits, std::string& why) {
    Bits b(src, n);
    out.assign(out_len, 0);
    size_t pos = 0;
    size_t remaining = 0;
    Huff temp, codes, offs;
    const int max_offs = (1 << offset_bits) - 1;
    auto fail = [&](const char* m) { why = m; return false; };
    while (pos < out_len) {
        if (remaining == 0) {
            remaining = b.read(16);
            // temp table
            int cnt = int(b.read(5));
            if (cnt == 0) temp.set_single(int(b.read(5)));
            else {
                uint8_t lens[kMaxTemp] = {};
                if (cnt > kMaxTemp) return fail("corrupted code table");
                for (int i = 0; i < cnt; ++i) {
                    int l = read_length_value(b);
                    if (l < 0) return fail("corrupted code table");
                    lens[i] = uint8_t(l);
                    if (i == 2) {
                        int skip = int(b.read(2));
                        for (int j = 0; j < skip; ++j) {
                            if (++i >= kMaxTemp) return fail("corrupted code table");
                            lens[i] = 0;
                        }
                    }
                }
                if (!temp.build(lens, cnt)) return fail("corrupted code table");
            }
            // literal/length table, coded with the temp table
            cnt = int(b.read(9));
            if (cnt == 0) codes.set_single(int(b.read(9)));
            else {
                uint8_t lens[kNumCodes] = {};
                if (cnt > kNumCodes) return fail("corrupted code table");
                int i = 0;
                while (i < cnt) {
                    int c = temp.decode(b);
                    if (c < 0) return fail("corrupted code table");
                    if (c <= 2) {
                        int skip = c == 0 ? 1 : c == 1 ? int(b.read(4)) + 3 : int(b.read(9)) + 20;
                        for (int j = 0; j < skip && i < cnt; ++j) lens[i++] = 0;
                    } else
                        lens[i++] = uint8_t(c - 2);
                }
                if (!codes.build(lens, cnt)) return fail("corrupted code table");
            }
            // offset table
            cnt = int(b.read(offset_bits));
            if (cnt == 0) offs.set_single(int(b.read(offset_bits)));
            else {
                uint8_t lens[32] = {};
                if (cnt > max_offs) return fail("corrupted offset table");
                for (int i = 0; i < cnt; ++i) {
                    int l = read_length_value(b);
                    if (l < 0) return fail("corrupted offset table");
                    lens[i] = uint8_t(l);
                }
                if (!offs.build(lens, cnt)) return fail("corrupted offset table");
            }
            if (b.over) return fail("packed data is truncated");
            if (remaining == 0) continue;
        }
        --remaining;
        int c = codes.decode(b);
        if (c < 0) return fail(b.over ? "packed data is truncated" : "invalid code in the packed data");
        if (c < 256) {
            out[pos++] = uint8_t(c);
            continue;
        }
        size_t len = size_t(c - 256 + kThreshold);
        int bits = offs.decode(b);
        if (bits < 0) return fail(b.over ? "packed data is truncated" : "invalid offset code in the packed data");
        size_t dist = size_t(bits);
        if (bits > 1) dist = (size_t(1) << (bits - 1)) + b.read(bits - 1);
        if (b.over) return fail("packed data is truncated");
        ++dist; // the stream stores the distance minus one
        for (size_t i = 0; i < len && pos < out_len; ++i, ++pos) out[pos] = pos >= dist ? out[pos - dist] : uint8_t(' ');
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------
// -lh1-: adaptive Huffman for the literal/length codes (a frequency-ordered tree rebuilt every 32 KB of
// frequency) and a static code for the high bits of the offset; 4 KB window.

class Lh1 {
public:
    static constexpr int kCodes = 314;
    static constexpr int kNodes = kCodes * 2 - 1;
    static constexpr unsigned kReorderLimit = 32 * 1024;
    static constexpr int kRing = 4096;

    Lh1() {
        std::memset(ring_, ' ', sizeof ring_);
        init_groups();
        init_tree();
        init_offset_table();
    }

    bool decode(const uint8_t* src, size_t n, size_t out_len, std::vector<uint8_t>& out, std::string& why) {
        Bits b(src, n);
        out.assign(out_len, 0);
        size_t pos = 0;
        auto put = [&](uint8_t v) {
            if (pos < out_len) out[pos++] = v;
            ring_[ring_pos_] = v;
            ring_pos_ = (ring_pos_ + 1) % kRing;
        };
        while (pos < out_len) {
            int node = 0;
            while (!nodes_[node].leaf) {
                int bit = b.bit();
                if (b.over) { why = "packed data is truncated"; return false; }
                node = nodes_[node].child - bit;
            }
            int code = nodes_[node].child;
            increment_for_code(code);
            if (code < 256) {
                put(uint8_t(code));
                continue;
            }
            unsigned fut = b.peek(8);
            unsigned offset = offset_lookup_[fut];
            b.skip(offset_len_[offset]);
            unsigned low = b.read(6);
            if (b.over) { why = "packed data is truncated"; return false; }
            offset = (offset << 6) | low;
            int count = code - 256 + 3;
            unsigned start = unsigned(ring_pos_) + kRing - offset - 1;
            for (int i = 0; i < count; ++i) put(ring_[(start + unsigned(i)) % kRing]);
        }
        return true;
    }

    // Encoder side of the same model; see lha_lh1_encode.
    void encode(const uint8_t* d, size_t n, std::vector<uint8_t>& out) {
        uint32_t acc = 0;
        int nbits = 0;
        auto put = [&](uint32_t v, int bits) {
            for (int i = bits - 1; i >= 0; --i) {
                acc = (acc << 1) | ((v >> i) & 1);
                if (++nbits == 8) { out.push_back(uint8_t(acc)); acc = 0; nbits = 0; }
            }
        };
        auto put_code = [&](int code) {
            std::vector<int> bits;
            for (uint16_t idx = leaf_nodes_[code]; idx != 0; idx = nodes_[idx].parent) {
                const Node& parent = nodes_[nodes_[idx].parent];
                bits.push_back(idx == parent.child ? 0 : 1);
            }
            for (size_t i = bits.size(); i-- > 0;) put(uint32_t(bits[i]), 1);
            increment_for_code(code);
        };
        out.clear();
        size_t pos = 0;
        while (pos < n) {
            size_t best_len = 0, best_dist = 0;
            size_t lo = pos > kRing ? pos - kRing : 0;
            for (size_t cand = pos; cand-- > lo;) {
                size_t len = 0;
                while (len < 60 && pos + len < n && d[cand + len] == d[pos + len]) ++len;
                if (len > best_len) { best_len = len; best_dist = pos - cand; }
                if (best_len == 60) break;
            }
            if (best_len >= 3) {
                put_code(256 + int(best_len) - 3);
                unsigned offset = unsigned(best_dist - 1);
                put(offset_code_[offset >> 6], offset_len_[offset >> 6]);
                put(offset & 63, 6);
                pos += best_len;
            } else {
                put_code(d[pos]);
                ++pos;
            }
        }
        if (nbits) put(0, 8 - nbits);
    }

private:
    struct Node {
        bool leaf = false;
        uint16_t child = 0, parent = 0, freq = 0, group = 0;
    };
    Node nodes_[kNodes];
    uint16_t leaf_nodes_[kCodes] = {};
    uint16_t groups_[kNodes] = {};
    unsigned num_groups_ = 0;
    uint16_t group_leader_[kNodes] = {};
    uint8_t offset_lookup_[256] = {};
    uint8_t offset_len_[64] = {};
    uint8_t offset_code_[64] = {}; // the code's value, right-aligned in offset_len_ bits
    uint8_t ring_[kRing];
    int ring_pos_ = 0;

    uint16_t alloc_group() { return groups_[num_groups_++]; }
    void free_group(uint16_t g) { groups_[--num_groups_] = g; }
    void init_groups() {
        for (int i = 0; i < kNodes; ++i) groups_[i] = uint16_t(i);
        num_groups_ = 0;
    }
    void init_tree() {
        int idx = kNodes - 1;
        uint16_t leaf_group = alloc_group();
        for (int i = 0; i < kCodes; ++i, --idx) {
            Node& nd = nodes_[idx];
            nd.leaf = true;
            nd.child = uint16_t(i);
            nd.freq = 1;
            nd.group = leaf_group;
            group_leader_[leaf_group] = uint16_t(idx);
            leaf_nodes_[i] = uint16_t(idx);
        }
        int child = kNodes - 1;
        for (; idx >= 0; --idx, child -= 2) {
            Node& nd = nodes_[idx];
            nd.leaf = false;
            nd.child = uint16_t(child);
            nodes_[child].parent = uint16_t(idx);
            nodes_[child - 1].parent = uint16_t(idx);
            nd.freq = uint16_t(nodes_[child].freq + nodes_[child - 1].freq);
            if (nd.freq == nodes_[idx + 1].freq) nd.group = nodes_[idx + 1].group;
            else nd.group = alloc_group();
            group_leader_[nd.group] = uint16_t(idx);
        }
    }
    // The static code for the top bits of an offset: 64 codes, 1 of 3 bits, 3 of 4, 8 of 5, 12 of 6, 24 of 7, 16 of 8.
    void init_offset_table() {
        static const unsigned fdist[] = {1, 3, 8, 12, 24, 16};
        unsigned code = 0, offset = 0;
        for (unsigned i = 0; i < 6; ++i) {
            unsigned len = i + 3, iter = 1u << (8 - len);
            for (unsigned j = 0; j < fdist[i]; ++j) {
                for (unsigned k = 0; k < iter; ++k) offset_lookup_[code | k] = uint8_t(offset);
                offset_len_[offset] = uint8_t(len);
                offset_code_[offset] = uint8_t(code >> (8 - len));
                code += iter;
                ++offset;
            }
        }
    }
    uint16_t make_group_leader(uint16_t index) {
        uint16_t group = nodes_[index].group;
        uint16_t leader_index = group_leader_[group];
        if (leader_index == index) return index;
        Node& node = nodes_[index];
        Node& leader = nodes_[leader_index];
        std::swap(leader.leaf, node.leaf);
        std::swap(leader.child, node.child);
        if (node.leaf) leaf_nodes_[node.child] = index;
        else {
            nodes_[node.child].parent = index;
            nodes_[node.child - 1].parent = index;
        }
        if (leader.leaf) leaf_nodes_[leader.child] = leader_index;
        else {
            nodes_[leader.child].parent = leader_index;
            nodes_[leader.child - 1].parent = leader_index;
        }
        return leader_index;
    }
    void increment_node_freq(uint16_t index) {
        Node& node = nodes_[index];
        Node& other = nodes_[index - 1];
        ++node.freq;
        if (index < kNodes - 1 && node.group == nodes_[index + 1].group) {
            ++group_leader_[node.group];
            if (node.freq == other.freq) node.group = other.group;
            else {
                node.group = alloc_group();
                group_leader_[node.group] = index;
            }
        } else if (node.freq == other.freq) {
            free_group(node.group);
            node.group = other.group;
        }
    }
    void reconstruct_tree() {
        Node* leaf = nodes_;
        for (int i = 0; i < kNodes; ++i) {
            if (nodes_[i].leaf) {
                leaf->leaf = true;
                leaf->child = nodes_[i].child;
                leaf->freq = uint16_t((nodes_[i].freq + 1) / 2);
                ++leaf;
            }
        }
        leaf = &nodes_[kCodes - 1];
        int child = kNodes - 1, i = kNodes - 1;
        while (i >= 0) {
            while (child - i < 2) {
                nodes_[i] = *leaf;
                leaf_nodes_[leaf->child] = uint16_t(i);
                --i;
                --leaf;
            }
            unsigned freq = unsigned(nodes_[child].freq) + nodes_[child - 1].freq;
            while (leaf >= nodes_ && freq >= leaf->freq) {
                nodes_[i] = *leaf;
                leaf_nodes_[leaf->child] = uint16_t(i);
                --i;
                --leaf;
            }
            nodes_[i].leaf = false;
            nodes_[i].freq = uint16_t(freq);
            nodes_[i].child = uint16_t(child);
            nodes_[child].parent = uint16_t(i);
            nodes_[child - 1].parent = uint16_t(i);
            --i;
            child -= 2;
        }
        init_groups();
        uint16_t group = alloc_group();
        nodes_[0].group = group;
        group_leader_[group] = 0;
        for (int k = 1; k < kNodes; ++k) {
            if (nodes_[k].freq == nodes_[k - 1].freq) nodes_[k].group = nodes_[k - 1].group;
            else {
                group = alloc_group();
                nodes_[k].group = group;
                group_leader_[group] = uint16_t(k);
            }
        }
    }
    void increment_for_code(int code) {
        if (nodes_[0].freq >= kReorderLimit) reconstruct_tree();
        ++nodes_[0].freq;
        uint16_t index = leaf_nodes_[code];
        while (index != 0) {
            index = make_group_leader(index);
            increment_node_freq(index);
            index = nodes_[index].parent;
        }
    }
};

// ---------------------------------------------------------------------------------------------------------
// Member headers

struct Header {
    uint8_t level = 0;
    std::string method, name, dir;
    uint32_t packed = 0, original = 0;
    uint16_t crc = 0;
    uint8_t os = 0;
    size_t data_pos = 0;
};

std::string normalise_path(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) s.push_back(p[i] == '\\' || p[i] == 0xFF ? '/' : char(p[i]));
    return s;
}

// Walks the extended-header chain whose first size field is at `f`. Each header is [size][type][data][next size]
// with `size` counting itself; the chain ends at a size of 0. Returns the position after the last size field, or
// npos on damage. `total` accumulates the bytes of all extended headers after the first size field.
size_t walk_ext(const std::vector<uint8_t>& d, size_t f, size_t limit, int field, Header& h, size_t& total) {
    total = 0;
    for (int guard = 0; guard < 4096; ++guard) {
        if (f + size_t(field) > limit) return std::string::npos;
        size_t len = field == 4 ? le32(&d[f]) : le16(&d[f]);
        if (len == 0) return f + size_t(field);
        if (len < size_t(field) + 1 || len > limit - f) return std::string::npos;
        uint8_t type = d[f + size_t(field)];
        const uint8_t* data = &d[f + size_t(field) + 1];
        size_t dlen = len - size_t(field) - 1;
        if (type == 0x01) h.name = normalise_path(data, dlen);
        else if (type == 0x02) {
            h.dir = normalise_path(data, dlen);
            if (!h.dir.empty() && h.dir.back() != '/') h.dir.push_back('/');
        }
        total += len;
        f += len;
    }
    return std::string::npos;
}

// Parses the header at `pos`; false with `why` when it is not a valid header.
bool parse_header(const std::vector<uint8_t>& d, size_t pos, Header& h, std::string& why) {
    if (pos + 22 > d.size()) { why = "truncated header"; return false; }
    h.level = d[pos + 20];
    const uint8_t* p = &d[pos];
    if (h.level == 0 || h.level == 1) {
        size_t hl = p[0];
        size_t min_len = h.level == 0 ? 22 : 25;
        if (hl < min_len) { why = "invalid header size"; return false; }
        size_t base_end = pos + 2 + hl;
        if (base_end > d.size()) { why = "truncated header"; return false; }
        unsigned sum = 0;
        for (size_t i = pos + 2; i < base_end; ++i) sum += d[i];
        if ((sum & 0xFF) != p[1]) { why = "header checksum mismatch"; return false; }
        if (p[2] != '-' || p[6] != '-') { why = "not an LHA header"; return false; }
        h.method.assign(reinterpret_cast<const char*>(p + 2), 5);
        h.packed = le32(p + 7);
        h.original = le32(p + 11);
        size_t nlen = p[21];
        if (min_len + nlen > hl) { why = "invalid file name length"; return false; }
        h.name = normalise_path(p + 22, nlen);
        h.crc = le16(p + 22 + nlen);
        if (h.level == 1) h.os = p[24 + nlen];
        if (h.level == 0) {
            h.data_pos = base_end;
        } else {
            size_t total = 0;
            size_t end = walk_ext(d, base_end - 2, d.size(), 2, h, total);
            if (end == std::string::npos) { why = "damaged extended header"; return false; }
            if (h.packed < total) { why = "invalid packed size"; return false; }
            h.packed -= uint32_t(total);
            h.data_pos = end;
        }
        return true;
    }
    if (h.level == 2) {
        size_t hl = le16(p);
        if (hl < 26 || pos + hl > d.size()) { why = "invalid or truncated header"; return false; }
        // LHA for OS-9/68k writes the header length two bytes short.
        if (p[23] == 'K' && pos + hl + 2 <= d.size()) hl += 2;
        if (p[2] != '-' || p[6] != '-') { why = "not an LHA header"; return false; }
        h.method.assign(reinterpret_cast<const char*>(p + 2), 5);
        h.packed = le32(p + 7);
        h.original = le32(p + 11);
        h.crc = le16(p + 21);
        h.os = p[23];
        size_t total = 0;
        if (walk_ext(d, pos + 24, pos + hl, 2, h, total) == std::string::npos) { why = "damaged extended header"; return false; }
        h.data_pos = pos + hl;
        return true;
    }
    if (h.level == 3) {
        if (pos + 32 > d.size() || le16(p) != 4) { why = "invalid level 3 header"; return false; }
        size_t hl = le32(p + 24);
        if (hl < 32 || hl > d.size() - pos) { why = "invalid or truncated header"; return false; }
        if (p[2] != '-' || p[6] != '-') { why = "not an LHA header"; return false; }
        h.method.assign(reinterpret_cast<const char*>(p + 2), 5);
        h.packed = le32(p + 7);
        h.original = le32(p + 11);
        h.crc = le16(p + 21);
        h.os = p[23];
        size_t total = 0;
        if (walk_ext(d, pos + 28, pos + hl, 4, h, total) == std::string::npos) { why = "damaged extended header"; return false; }
        h.data_pos = pos + hl;
        return true;
    }
    why = "unknown header level " + std::to_string(h.level);
    return false;
}

} // namespace

uint16_t lha_crc16(const uint8_t* data, size_t n) {
    static uint16_t table[256];
    static bool init = false;
    if (!init) {
        for (unsigned i = 0; i < 256; ++i) {
            unsigned c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xA001u : c >> 1;
            table[i] = uint16_t(c);
        }
        init = true;
    }
    unsigned crc = 0;
    for (size_t i = 0; i < n; ++i) crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF];
    return uint16_t(crc);
}

bool lha_lh1_encode(const uint8_t* data, size_t n, std::vector<uint8_t>& out) {
    auto enc = std::make_unique<Lh1>();
    enc->encode(data, n, out);
    return true;
}

bool lha_decode(const std::string& method, const uint8_t* src, size_t n, size_t out_len, std::vector<uint8_t>& out,
                std::string& why) {
    if (method == "-lh0-" || method == "-lz4-") {
        if (n != out_len) { why = "stored member whose packed and original sizes differ"; return false; }
        out.assign(src, src + n);
        return true;
    }
    if (method == "-lh1-") {
        auto dec = std::make_unique<Lh1>();
        return dec->decode(src, n, out_len, out, why);
    }
    if (method == "-lh4-" || method == "-lh5-") return decode_new(src, n, out_len, out, 4, why);
    if (method == "-lh6-" || method == "-lh7-") return decode_new(src, n, out_len, out, 5, why);
    why = "compression method " + method + " is not supported";
    return false;
}

bool read_lha(const std::vector<uint8_t>& d, LhaArchive& out, std::string& why, size_t max_member, size_t max_total) {
    out = LhaArchive();
    size_t pos = 0, total = 0;
    while (pos < d.size() && d[pos] != 0) {
        Header h;
        std::string herr;
        if (!parse_header(d, pos, h, herr)) {
            if (out.entries.empty()) { why = "not a valid LHA archive (" + herr + ")"; return false; }
            out.note = "data after the last valid member ignored (" + herr + ")";
            break;
        }
        // The DOS tool LHARK writes its own, incompatible -lh7- (level 1, OS type ' '); name it apart.
        if (h.level == 1 && h.os == ' ' && h.method == "-lh7-") h.method = "-lk7-";
        LhaEntry e;
        e.name = h.dir + h.name;
        e.method = h.method;
        e.header_level = h.level;
        e.packed_size = h.packed;
        e.original_size = h.original;
        e.crc16 = h.crc;
        e.is_dir = h.method == "-lhd-";
        bool truncated = h.data_pos > d.size() || h.packed > d.size() - h.data_pos;
        if (e.is_dir) {
            // no data
        } else if (truncated) {
            e.problem = "truncated (the archive ends inside this member)";
        } else if (h.original > max_member) {
            e.problem = "too large (" + std::to_string(h.original) + " bytes)";
        } else if (h.method != "-lh0-" && h.method != "-lz4-" && h.method != "-lh1-" && h.method != "-lh4-" &&
                   h.method != "-lh5-" && h.method != "-lh6-" && h.method != "-lh7-") {
            e.problem = "compression method " + h.method + " is not supported" + (h.method == "-lk7-" ? " (LHARK's own -lh7-)" : "");
            e.unsupported = true;
        } else {
            total += h.original;
            if (total > max_total) { why = "archive too large when unpacked"; return false; }
            std::string derr;
            if (!lha_decode(h.method, d.data() + h.data_pos, h.packed, h.original, e.data, derr)) {
                e.problem = derr;
                e.data.clear();
            } else if (lha_crc16(e.data.data(), e.data.size()) != h.crc) {
                e.problem = "CRC mismatch (the member is corrupted)";
                e.data.clear();
            }
        }
        out.entries.push_back(std::move(e));
        if (truncated) { out.note = "the archive is truncated"; break; }
        pos = h.data_pos + h.packed;
    }
    if (out.entries.empty()) { why = "empty LHA archive"; return false; }
    return true;
}

} // namespace x2::import
