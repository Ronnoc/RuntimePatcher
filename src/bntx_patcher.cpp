// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — In-Memory SARC.zst + BNTX Texture Cloner & Patricia Trie Builder
#include "fefw_foundation/bntx_patcher.h"
#include "fefw_foundation/msbt_patcher.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace fefw_foundation {
namespace {

uint16_t u16_le(const uint8_t* p) {
    uint16_t v = 0;
    std::memcpy(&v, p, 2);
    return v;
}
uint32_t u32_le(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}
int32_t s32_le(const uint8_t* p) {
    int32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}
uint64_t u64_le(const uint8_t* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, 8);
    return v;
}
int64_t s64_le(const uint8_t* p) {
    int64_t v = 0;
    std::memcpy(&v, p, 8);
    return v;
}

void put_u16(std::vector<uint8_t>& b, uint16_t v) {
    uint8_t t[2];
    std::memcpy(t, &v, 2);
    b.insert(b.end(), t, t + 2);
}
void put_u32(std::vector<uint8_t>& b, uint32_t v) {
    uint8_t t[4];
    std::memcpy(t, &v, 4);
    b.insert(b.end(), t, t + 4);
}
void put_s32(std::vector<uint8_t>& b, int32_t v) {
    uint8_t t[4];
    std::memcpy(t, &v, 4);
    b.insert(b.end(), t, t + 4);
}
void put_u64(std::vector<uint8_t>& b, uint64_t v) {
    uint8_t t[8];
    std::memcpy(t, &v, 8);
    b.insert(b.end(), t, t + 8);
}
void put_s64(std::vector<uint8_t>& b, int64_t v) {
    uint8_t t[8];
    std::memcpy(t, &v, 8);
    b.insert(b.end(), t, t + 8);
}

int64_t round_up(int64_t x, int64_t y) {
    if (y <= 1) return x;
    return ((x - 1) | (y - 1)) + 1;
}

// Byte offset of (x_bytes, y) in a block-linear (swizzled) Switch 2 texture.
// A GOB is 64 bytes x 8 rows (512 bytes); inside it the address bits are, from low to high,
// x[3:0] y[0] y[1] x[4] y[2] x[5]. GOBs stack (1 << block_height_log2) high into blocks, and blocks fill the
// surface left to right, then top to bottom.
int block_linear_offset(int x_bytes, int y, int row_bytes, int block_height_log2) {
    const int block_rows = 8 << block_height_log2;
    const int block_bytes = 512 << block_height_log2;
    const int blocks_across = (row_bytes + 63) / 64;
    const int block = (y / block_rows) * blocks_across + x_bytes / 64;
    const int gob = (y % block_rows) / 8;
    const int in_gob = (x_bytes & 15) | (y & 1) << 4 | ((y >> 1) & 1) << 5 | ((x_bytes >> 4) & 1) << 6 |
                       ((y >> 2) & 1) << 7 | ((x_bytes >> 5) & 1) << 8;
    return block * block_bytes + gob * 512 + in_gob;
}

// ============================================================================
// ResDict (Patricia trie) builder. Algorithm from BfresLibrary's ResDictUpdate.cs (MIT, see
// THIRD_PARTY_NOTICES.md).
// ============================================================================
struct BigBits {
    // Represents big-endian UTF-8 string bits as a vector of bits from LSB (bit 0) to MSB.
    // In the reference implementation:
    //   string bits = string.Join("", encoding.GetBytes(text).Select(n => Convert.ToString(n, 2).PadLeft(8, '0')));
    //   BigInteger data = bits.Aggregate(new BigInteger(), (b, c) => b * 2 + c - '0');
    // That means the last bit of the last byte is bit 0 (LSB), and the highest bit of the first byte is the MSB!
    std::vector<uint8_t> bytes_be;  // UTF-8 bytes in original order

    static BigBits from_utf8(const std::string& s) {
        BigBits b;
        b.bytes_be.assign(s.begin(), s.end());
        return b;
    }

    int bit_length() const {
        if (bytes_be.empty()) return 1;
        // Find first non-zero byte
        size_t first_nz = 0;
        while (first_nz < bytes_be.size() && bytes_be[first_nz] == 0) first_nz++;
        if (first_nz == bytes_be.size()) return 1;
        uint8_t top = bytes_be[first_nz];
        int top_bits = 0;
        while (top > 0) {
            top >>= 1;
            top_bits++;
        }
        return static_cast<int>((bytes_be.size() - 1 - first_nz) * 8 + top_bits);
    }

    int get_bit(int b) const {
        if (b < 0) return 0;
        size_t byte_from_end = static_cast<size_t>(b / 8);
        if (byte_from_end >= bytes_be.size()) return 0;
        uint8_t byte_val = bytes_be[bytes_be.size() - 1 - byte_from_end];
        return (byte_val >> (b % 8)) & 1;
    }

    int first_1bit() const {
        int bl = bit_length();
        for (int i = 0; i < bl; i++) {
            if (get_bit(i) == 1) return i;
        }
        return 0;
    }

    static int bit_mismatch(const BigBits& a, const BigBits& b) {
        int m = std::max(a.bit_length(), b.bit_length());
        for (int i = 0; i < m; i++) {
            if (a.get_bit(i) != b.get_bit(i)) return i;
        }
        return -1;
    }

    bool operator<(const BigBits& o) const {
        if (bytes_be.size() != o.bytes_be.size()) return bytes_be.size() < o.bytes_be.size();
        return bytes_be < o.bytes_be;
    }
    bool operator==(const BigBits& o) const {
        return bytes_be == o.bytes_be;
    }
};

struct DictNodeOut {
    uint32_t reference = 0xFFFFFFFFu;
    uint16_t left = 0;
    uint16_t right = 0;
    std::string key;
};

std::vector<DictNodeOut> build_res_dict(const std::vector<std::string>& keys) {
    struct TrieNode {
        int child[2] = {0, 0};
        int parent = 0;
        int bit_inx = -1;
        BigBits data;
        std::string key;
    };

    std::vector<TrieNode> pool;
    pool.reserve(keys.size() + 1);

    // Node 0 = root
    TrieNode root;
    root.child[0] = 0;
    root.child[1] = 0;
    root.parent = 0;
    root.bit_inx = -1;
    pool.push_back(root);

    auto search = [&](const BigBits& data, bool prev) -> int {
        if (pool[0].child[0] == 0) return 0;
        int node = pool[0].child[0];
        int prev_node = node;
        while (true) {
            prev_node = node;
            int dir = data.get_bit(pool[node].bit_inx);
            node = pool[node].child[dir];
            if (pool[node].bit_inx <= pool[prev_node].bit_inx) break;
        }
        return prev ? prev_node : node;
    };

    for (const auto& k : keys) {
        BigBits data = BigBits::from_utf8(k);
        int current = search(data, true);
        int bit_idx = BigBits::bit_mismatch(pool[current].data, data);

        while (bit_idx < pool[pool[current].parent].bit_inx) {
            current = pool[current].parent;
        }

        int new_idx = static_cast<int>(pool.size());
        TrieNode new_node;
        new_node.child[0] = new_idx;
        new_node.child[1] = new_idx;
        new_node.data = data;
        new_node.key = k;

        if (bit_idx < pool[current].bit_inx) {
            new_node.bit_inx = bit_idx;
            new_node.parent = pool[current].parent;
            new_node.child[data.get_bit(bit_idx) ^ 1] = current;
            int p = pool[current].parent;
            pool[p].child[data.get_bit(pool[p].bit_inx)] = new_idx;
            pool[current].parent = new_idx;
            pool.push_back(std::move(new_node));
        } else if (bit_idx > pool[current].bit_inx) {
            new_node.bit_inx = bit_idx;
            new_node.parent = current;
            if (pool[current].data.get_bit(bit_idx) == (data.get_bit(bit_idx) ^ 1)) {
                new_node.child[data.get_bit(bit_idx) ^ 1] = current;
            } else {
                new_node.child[data.get_bit(bit_idx) ^ 1] = 0;
            }
            pool[current].child[data.get_bit(pool[current].bit_inx)] = new_idx;
            pool.push_back(std::move(new_node));
        } else {
            int new_bit_idx = data.first_1bit();
            int c_next = pool[current].child[data.get_bit(bit_idx)];
            if (c_next != 0) {
                new_bit_idx = BigBits::bit_mismatch(pool[c_next].data, data);
            }
            new_node.bit_inx = new_bit_idx;
            new_node.parent = current;
            new_node.child[data.get_bit(new_bit_idx) ^ 1] = c_next;
            pool[current].child[data.get_bit(bit_idx)] = new_idx;
            pool.push_back(std::move(new_node));
        }
    }

    std::vector<DictNodeOut> out(pool.size());
    for (size_t i = 0; i < pool.size(); i++) {
        const auto& n = pool[i];
        int byte_idx = n.bit_inx / 8;
        int compact_bit = (byte_idx << 3) | (n.bit_inx - 8 * byte_idx);
        out[i].reference = static_cast<uint32_t>(compact_bit);
        out[i].left = static_cast<uint16_t>(n.child[0]);
        out[i].right = static_cast<uint16_t>(n.child[1]);
        out[i].key = (i == 0) ? "" : n.key;
    }
    return out;
}

// ============================================================================
// BNTX container structures (layout as documented by BNTX Editor, see THIRD_PARTY_NOTICES.md)
// ============================================================================
struct BntxTexture {
    uint8_t flags = 0;
    uint8_t dim = 0;
    uint16_t tile_mode = 0;
    uint16_t swizzle = 0;
    uint16_t num_mips = 1;
    uint16_t num_samples = 1;
    uint32_t format = 0;
    uint32_t access_flags = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t depth = 1;
    uint32_t array_length = 1;
    uint32_t texture_layout = 0;
    uint32_t texture_layout2 = 0;
    uint32_t image_size = 0;
    uint32_t alignment = 512;
    uint32_t comp_sel = 0;
    uint8_t img_dim = 0;
    int64_t name_addr = 0;
    int64_t parent_addr = 0;
    int64_t ptrs_addr = 0;
    int64_t user_data_addr = 0;
    int64_t tex_ptr = 0;
    int64_t tex_view_ptr = 0;
    int64_t desc_slot_data_addr = 0;
    int64_t user_dict_addr = 0;

    int name_idx = -1;
    std::string name;
    int64_t pos = 0;
    std::vector<int64_t> mip_offsets;
    std::vector<uint8_t> data;

    void load(const uint8_t* buf, size_t pos_in) {
        pos = static_cast<int64_t>(pos_in);
        // Format: '2B4H2x2I3i3I20x3IB3x8q' (total 0x90 = 144 bytes)
        flags = buf[pos_in + 0];
        dim = buf[pos_in + 1];
        tile_mode = u16_le(buf + pos_in + 2);
        swizzle = u16_le(buf + pos_in + 4);
        num_mips = u16_le(buf + pos_in + 6);
        num_samples = u16_le(buf + pos_in + 8);
        // 2x padding at +10..12
        format = u32_le(buf + pos_in + 12);
        access_flags = u32_le(buf + pos_in + 16);
        width = s32_le(buf + pos_in + 20);
        height = s32_le(buf + pos_in + 24);
        depth = s32_le(buf + pos_in + 28);
        array_length = u32_le(buf + pos_in + 32);
        texture_layout = u32_le(buf + pos_in + 36);
        texture_layout2 = u32_le(buf + pos_in + 40);
        // 20x padding at +44..64
        image_size = u32_le(buf + pos_in + 64);
        alignment = u32_le(buf + pos_in + 68);
        comp_sel = u32_le(buf + pos_in + 72);
        img_dim = buf[pos_in + 76];
        // 3x padding at +77..80
        name_addr = s64_le(buf + pos_in + 80);
        parent_addr = s64_le(buf + pos_in + 88);
        ptrs_addr = s64_le(buf + pos_in + 96);
        user_data_addr = s64_le(buf + pos_in + 104);
        tex_ptr = s64_le(buf + pos_in + 112);
        tex_view_ptr = s64_le(buf + pos_in + 120);
        desc_slot_data_addr = s64_le(buf + pos_in + 128);
        user_dict_addr = s64_le(buf + pos_in + 136);

        int64_t first_mip = s64_le(buf + static_cast<size_t>(ptrs_addr));
        mip_offsets.assign(1, 0);
        for (int i = 1; i < num_mips; i++) {
            mip_offsets.push_back(s64_le(buf + static_cast<size_t>(ptrs_addr + 8 * i)) - first_mip);
        }
        data.assign(buf + static_cast<size_t>(first_mip), buf + static_cast<size_t>(first_mip + image_size));
    }

    std::vector<uint8_t> save() const {
        std::vector<uint8_t> out;
        out.reserve(144);
        out.push_back(flags);
        out.push_back(dim);
        put_u16(out, tile_mode);
        put_u16(out, swizzle);
        put_u16(out, num_mips);
        put_u16(out, num_samples);
        put_u16(out, 0);  // 2x
        put_u32(out, format);
        put_u32(out, access_flags);
        put_s32(out, width);
        put_s32(out, height);
        put_s32(out, depth);
        put_u32(out, array_length);
        put_u32(out, texture_layout);
        put_u32(out, texture_layout2);
        out.insert(out.end(), 20, 0);  // 20x
        put_u32(out, image_size);
        put_u32(out, alignment);
        put_u32(out, comp_sel);
        out.push_back(img_dim);
        out.insert(out.end(), 3, 0);  // 3x
        put_s64(out, name_addr);
        put_s64(out, parent_addr);
        put_s64(out, ptrs_addr);
        put_s64(out, user_data_addr);
        put_s64(out, tex_ptr);
        put_s64(out, tex_view_ptr);
        put_s64(out, desc_slot_data_addr);
        put_s64(out, user_dict_addr);
        return out;
    }
};

struct RelocEntry {
    uint32_t pos = 0;
    uint16_t struct_count = 0;
    uint8_t offset_count = 0;
    uint8_t padding_count = 0;
};

struct RelocBlock {
    uint64_t base_ptr = 0;
    uint32_t pos = 0;
    uint32_t size = 0;
    int32_t entry_idx = 0;
    int32_t entry_count = 0;
};

std::vector<uint8_t> build_sarc(const std::vector<uint8_t>& bntx_bytes) {
    const char fn[] = "timg/__Combined.bntx";
    uint32_t data_offset = 0x1000;
    uint32_t file_size = data_offset + static_cast<uint32_t>(bntx_bytes.size());

    std::vector<uint8_t> out;
    out.reserve(file_size);
    // SARC header (0x14 bytes)
    out.insert(out.end(), {'S', 'A', 'R', 'C'});
    put_u16(out, 0x14);
    put_u16(out, 0xFEFF);
    put_u32(out, file_size);
    put_u32(out, data_offset);
    put_u16(out, 0x0100);
    put_u16(out, 0x0000);

    // SFAT header (0x0C bytes)
    out.insert(out.end(), {'S', 'F', 'A', 'T'});
    put_u16(out, 0x0C);
    put_u16(out, 1);
    put_u32(out, 0x65);

    // SFAT node (0x10 bytes)
    put_u32(out, 0xc3525d09u);
    put_u32(out, 0x01000000u);
    put_u32(out, 0u);
    put_u32(out, static_cast<uint32_t>(bntx_bytes.size()));

    // SFNT header (0x08 bytes)
    out.insert(out.end(), {'S', 'F', 'N', 'T'});
    put_u16(out, 0x08);
    put_u16(out, 0x00);

    out.insert(out.end(), fn, fn + sizeof(fn));  // includes null terminator
    if (out.size() < data_offset) {
        out.resize(data_offset, 0);
    }
    out.insert(out.end(), bntx_bytes.begin(), bntx_bytes.end());
    return out;
}

}  // namespace

std::vector<uint8_t> swizzle_rgba_switch2(const uint8_t* linear_rgba, int width, int height, int block_height_log2) {
    int bpp = 4;
    std::vector<uint8_t> swizzled(static_cast<size_t>(width * height * bpp), 0);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int addr = block_linear_offset(x * bpp, y, width * bpp, block_height_log2);
            int src_off = (y * width + x) * bpp;
            if (addr >= 0 && static_cast<size_t>(addr + bpp) <= swizzled.size()) {
                std::memcpy(swizzled.data() + addr, linear_rgba + src_off, static_cast<size_t>(bpp));
            }
        }
    }
    return swizzled;
}

std::vector<uint8_t> swizzle_bc7_switch2(const uint8_t* linear_bc7, int width, int height, int block_height_log2) {
    int bw = (width + 3) / 4;
    int bh = (height + 3) / 4;
    int max_addr = 0;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            int addr = block_linear_offset(bx * 16, by, bw * 16, block_height_log2);
            if (addr + 16 > max_addr) max_addr = addr + 16;
        }
    }
    size_t buf_size = static_cast<size_t>(round_up(max_addr, 512));
    std::vector<uint8_t> swizzled(buf_size, 0);
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            int off = block_linear_offset(bx * 16, by, bw * 16, block_height_log2);
            int src_off = (by * bw + bx) * 16;
            std::memcpy(swizzled.data() + off, linear_bc7 + src_off, 16);
        }
    }
    return swizzled;
}

bool patch_sarc_bntx_archive(const std::vector<uint8_t>& input_arc_zst,
                             const std::vector<BntxTexturePatch>& patches,
                             std::vector<uint8_t>& output_arc_zst,
                             std::string* err) {
    if (patches.empty()) {
        output_arc_zst = input_arc_zst;
        return true;
    }

    std::vector<uint8_t> arc_data;
    if (!zstd_decompress(input_arc_zst.data(), input_arc_zst.size(), arc_data, err)) return false;
    if (arc_data.size() < 0x20 || std::memcmp(arc_data.data(), "SARC", 4) != 0) {
        if (err) *err = "Invalid SARC header in .arc.zst";
        return false;
    }
    uint32_t data_offset = u32_le(arc_data.data() + 0x0C);
    if (data_offset >= arc_data.size()) {
        if (err) *err = "Invalid SARC data_offset";
        return false;
    }

    const uint8_t* bntx = arc_data.data() + data_offset;
    size_t bntx_len = arc_data.size() - data_offset;
    if (bntx_len < 64 || std::memcmp(bntx, "BNTX\0\0\0\0", 8) != 0) {
        if (err) *err = "Invalid BNTX magic inside SARC";
        return false;
    }

    // Parse BNTX Header
    uint32_t file_name_addr = u32_le(bntx + 16);
    uint16_t first_blk_addr = u16_le(bntx + 22);

    // Parse TexContainer at +32
    char target[4];
    std::memcpy(target, bntx + 32, 4);
    uint32_t tex_count = u32_le(bntx + 36);
    int64_t info_ptrs_addr = s64_le(bntx + 40);

    // Parse StringTable at first_blk_addr + 16
    int64_t str_tbl_pos = static_cast<int64_t>(first_blk_addr) + 16;
    uint32_t str_count = u32_le(bntx + static_cast<size_t>(str_tbl_pos));
    struct StrEntry {
        int64_t pos = 0;
        std::string str;
    };
    std::vector<StrEntry> str_entries;
    int64_t cur_str_pos = str_tbl_pos + 8;
    for (uint32_t i = 0; i < str_count; i++) {
        uint16_t slen = u16_le(bntx + static_cast<size_t>(cur_str_pos));
        std::string s(reinterpret_cast<const char*>(bntx + static_cast<size_t>(cur_str_pos + 2)), slen);
        str_entries.push_back({cur_str_pos, std::move(s)});
        cur_str_pos += slen + 3;
        cur_str_pos = ((cur_str_pos - 1) | 1) + 1;
    }

    auto find_str_idx_by_pos = [&](int64_t p) -> int {
        for (size_t i = 0; i < str_entries.size(); i++) {
            if (str_entries[i].pos == p) return static_cast<int>(i);
        }
        return -1;
    };
    auto find_str_idx_by_val = [&](const std::string& s) -> int {
        for (size_t i = 0; i < str_entries.size(); i++) {
            if (str_entries[i].str == s) return static_cast<int>(i);
        }
        return -1;
    };

    int header_name_idx = find_str_idx_by_pos(static_cast<int64_t>(file_name_addr) - 2);

    // Parse Textures
    std::vector<BntxTexture> textures(tex_count);
    for (uint32_t i = 0; i < tex_count; i++) {
        int64_t tpos = s64_le(bntx + static_cast<size_t>(info_ptrs_addr + 8 * i));
        textures[i].load(bntx, static_cast<size_t>(tpos + 16));
        textures[i].name_idx = find_str_idx_by_pos(textures[i].name_addr);
        if (textures[i].name_idx >= 0) {
            textures[i].name = str_entries[static_cast<size_t>(textures[i].name_idx)].str;
        }
    }

    // Apply all texture patches (clone / update)
    for (const auto& patch : patches) {
        auto it_existing = std::find_if(textures.begin(), textures.end(),
                                        [&](const BntxTexture& t) { return t.name == patch.new_name; });
        if (it_existing != textures.end()) {
            if (!patch.custom_swizzled_data.empty()) {
                it_existing->data = patch.custom_swizzled_data;
                it_existing->image_size = static_cast<uint32_t>(patch.custom_swizzled_data.size());
            }
            continue;
        }

        auto it_src = std::find_if(textures.begin(), textures.end(),
                                   [&](const BntxTexture& t) { return t.name == patch.clone_from_name; });
        if (it_src == textures.end()) {
            if (err) *err = "Source texture '" + patch.clone_from_name + "' not found in BNTX";
            return false;
        }

        BntxTexture cloned = *it_src;
        str_entries.push_back({0, patch.new_name});
        cloned.name_idx = static_cast<int>(str_entries.size()) - 1;
        cloned.name = patch.new_name;
        if (!patch.custom_swizzled_data.empty()) {
            cloned.data = patch.custom_swizzled_data;
            cloned.image_size = static_cast<uint32_t>(patch.custom_swizzled_data.size());
        }
        textures.push_back(std::move(cloned));
    }

    // Rebuild Patricia trie dictionary
    std::vector<std::string> all_keys;
    all_keys.reserve(textures.size());
    for (const auto& t : textures) all_keys.push_back(t.name);
    std::vector<DictNodeOut> dict_nodes = build_res_dict(all_keys);

    // Serialize the BNTX container
    uint32_t total_tex = static_cast<uint32_t>(textures.size());
    std::vector<RelocEntry> reloc_entries;
    reloc_entries.push_back({0x28, 2, 1, 1});
    reloc_entries.push_back({0x40, 1, 1, 0});

    uint32_t rpos = 0x198;
    uint32_t rcount = total_tex;
    while (rcount > 0) {
        uint8_t step = static_cast<uint8_t>(std::min<uint32_t>(rcount, 0xFF));
        reloc_entries.push_back({rpos, 1, step, 0});
        rpos += step * 8;
        rcount -= step;
    }

    uint8_t align_shift = (std::memcmp(target, "NX  ", 4) == 0 || std::memcmp(target, "Ounc", 4) == 0) ? 0x0C : 3;
    int64_t new_str_tbl_pos = 0x198 + 8 * static_cast<int64_t>(total_tex) + 16;

    std::vector<uint8_t> str_tbl_bytes;
    put_u32(str_tbl_bytes, static_cast<uint32_t>(str_entries.size()));
    put_u32(str_tbl_bytes, 0);
    int64_t entries_cursor = new_str_tbl_pos + 8;
    for (auto& se : str_entries) {
        se.pos = entries_cursor;
        uint16_t slen = static_cast<uint16_t>(se.str.size());
        put_u16(str_tbl_bytes, slen);
        str_tbl_bytes.insert(str_tbl_bytes.end(), se.str.begin(), se.str.end());
        str_tbl_bytes.push_back(0);
        entries_cursor += slen + 3;
        int64_t aligned_cursor = ((entries_cursor - 1) | 1) + 1;
        if (aligned_cursor > entries_cursor) {
            str_tbl_bytes.insert(str_tbl_bytes.end(), static_cast<size_t>(aligned_cursor - entries_cursor), 0);
            entries_cursor = aligned_cursor;
        }
    }

    auto get_pos_from_idx = [&](int idx) -> int64_t {
        if (idx < 0) return new_str_tbl_pos + 4;
        return str_entries[static_cast<size_t>(idx)].pos;
    };

    uint32_t new_file_name_addr = static_cast<uint32_t>(get_pos_from_idx(header_name_idx) + 2);
    uint16_t new_first_blk_addr = static_cast<uint16_t>(new_str_tbl_pos - 16);

    int64_t dict_unaligned_pos = new_str_tbl_pos + static_cast<int64_t>(str_tbl_bytes.size());
    int64_t dict_pos = round_up(dict_unaligned_pos, 8);
    std::vector<uint8_t> tex_name_dict_bytes(static_cast<size_t>(dict_pos - dict_unaligned_pos), 0);
    tex_name_dict_bytes.insert(tex_name_dict_bytes.end(), {'_', 'D', 'I', 'C'});
    put_u32(tex_name_dict_bytes, total_tex);
    for (size_t i = 0; i < dict_nodes.size(); i++) {
        put_u32(tex_name_dict_bytes, dict_nodes[i].reference);
        put_u16(tex_name_dict_bytes, dict_nodes[i].left);
        put_u16(tex_name_dict_bytes, dict_nodes[i].right);
        int s_idx = (i == 0) ? -1 : find_str_idx_by_val(dict_nodes[i].key);
        put_s64(tex_name_dict_bytes, get_pos_from_idx(s_idx));
    }

    rpos = static_cast<uint32_t>(dict_pos + 16);
    rcount = total_tex + 1;
    while (rcount > 0) {
        uint8_t step = static_cast<uint8_t>(std::min<uint32_t>(rcount, 0xFF));
        reloc_entries.push_back({rpos, 1, step, 0});
        rpos += step * 8;
        rcount -= step;
    }

    uint32_t str_blk_size = static_cast<uint32_t>(str_tbl_bytes.size() + tex_name_dict_bytes.size() + 16);
    std::vector<uint8_t> str_tbl_hdr_bytes;
    str_tbl_hdr_bytes.insert(str_tbl_hdr_bytes.end(), {'_', 'S', 'T', 'R'});
    put_u32(str_tbl_hdr_bytes, str_blk_size);
    put_u32(str_tbl_hdr_bytes, str_blk_size);
    put_u32(str_tbl_hdr_bytes, 0);

    int64_t info_blks_pos = new_str_tbl_pos - 16 + str_blk_size;
    std::vector<uint8_t> info_blks;

    int64_t data_blk_pos = info_blks_pos + 16;
    for (uint32_t i = 0; i < total_tex; i++) {
        data_blk_pos += 0x2A0 + 8 * textures[i].num_mips;
    }

    uint32_t block0_size = static_cast<uint32_t>(data_blk_pos - 16);
    int64_t data_alignment = 1LL << align_shift;
    int64_t aligned_data_blk_pos = round_up(data_blk_pos, data_alignment);
    size_t data_blk_align_len = static_cast<size_t>(aligned_data_blk_pos - data_blk_pos);
    data_blk_pos = aligned_data_blk_pos;
    int64_t container_data_blk_addr = data_blk_pos - 16;

    std::vector<uint8_t> data_blk_body;
    for (uint32_t i = 0; i < total_tex; i++) {
        auto& tex = textures[i];
        tex.pos = info_blks_pos + static_cast<int64_t>(info_blks.size()) + 16;
        tex.name_addr = get_pos_from_idx(tex.name_idx);
        tex.parent_addr = 0x20;
        tex.ptrs_addr = tex.pos + 0x290;
        tex.user_data_addr = 0;
        tex.tex_ptr = tex.pos + 0x90;
        tex.tex_view_ptr = tex.pos + 0x190;
        tex.desc_slot_data_addr = 0;
        tex.user_dict_addr = 0;

        reloc_entries.push_back({static_cast<uint32_t>(tex.pos + 0x50), 1, 3, 0});
        reloc_entries.push_back({static_cast<uint32_t>(tex.pos + 0x70), 1, 2, 0});

        uint32_t brti_size = 0x2A0 + 8 * tex.num_mips;
        if (i == total_tex - 1) {
            brti_size += static_cast<uint32_t>(data_blk_align_len);
        }
        info_blks.insert(info_blks.end(), {'B', 'R', 'T', 'I'});
        put_u32(info_blks, brti_size);
        put_u32(info_blks, brti_size);
        put_u32(info_blks, 0);

        auto saved_tex = tex.save();
        info_blks.insert(info_blks.end(), saved_tex.begin(), saved_tex.end());
        info_blks.insert(info_blks.end(), 0x200, 0);

        int64_t cur_data_pos = data_blk_pos + static_cast<int64_t>(data_blk_body.size());
        int64_t aligned_tex_data_pos = round_up(cur_data_pos, tex.alignment);
        if (aligned_tex_data_pos > cur_data_pos) {
            data_blk_body.insert(data_blk_body.end(), static_cast<size_t>(aligned_tex_data_pos - cur_data_pos), 0);
            cur_data_pos = aligned_tex_data_pos;
        }

        for (int64_t m_off : tex.mip_offsets) {
            put_s64(info_blks, cur_data_pos + m_off);
        }
        data_blk_body.insert(data_blk_body.end(), tex.data.begin(), tex.data.end());
    }

    int64_t unaligned_reloc_addr = data_blk_pos + static_cast<int64_t>(data_blk_body.size());
    int64_t reloc_addr = round_up(unaligned_reloc_addr, data_alignment);
    if (reloc_addr > unaligned_reloc_addr) {
        data_blk_body.insert(data_blk_body.end(), static_cast<size_t>(reloc_addr - unaligned_reloc_addr), 0);
    }

    uint32_t brtd_block_size = static_cast<uint32_t>(data_blk_body.size() + 16);
    std::vector<uint8_t> data_blk;
    data_blk.insert(data_blk.end(), data_blk_align_len, 0);
    data_blk.insert(data_blk.end(), {'B', 'R', 'T', 'D'});
    put_u32(data_blk, 0);
    put_u32(data_blk, brtd_block_size);
    put_u32(data_blk, 0);
    data_blk.insert(data_blk.end(), data_blk_body.begin(), data_blk_body.end());

    int32_t blk0_entry_count = static_cast<int32_t>(reloc_entries.size());
    reloc_entries.push_back({0x30, 1, 1, 0});
    for (const auto& tex : textures) {
        rpos = static_cast<uint32_t>(tex.pos + 0x290);
        rcount = tex.num_mips;
        while (rcount > 0) {
            uint8_t step = static_cast<uint8_t>(std::min<uint32_t>(rcount, 0xFF));
            reloc_entries.push_back({rpos, 1, step, 0});
            rpos += step * 8;
            rcount -= step;
        }
    }
    int32_t blk1_entry_count = static_cast<int32_t>(reloc_entries.size()) - blk0_entry_count;

    std::vector<uint8_t> reloc_tbl_bytes;
    reloc_tbl_bytes.insert(reloc_tbl_bytes.end(), {'_', 'R', 'L', 'T'});
    put_u32(reloc_tbl_bytes, static_cast<uint32_t>(reloc_addr));
    put_u32(reloc_tbl_bytes, 2);
    put_u32(reloc_tbl_bytes, 0);

    // Block 0
    put_u64(reloc_tbl_bytes, 0);
    put_u32(reloc_tbl_bytes, 0);
    put_u32(reloc_tbl_bytes, block0_size);
    put_s32(reloc_tbl_bytes, 0);
    put_s32(reloc_tbl_bytes, blk0_entry_count);

    // Block 1
    put_u64(reloc_tbl_bytes, 0);
    put_u32(reloc_tbl_bytes, static_cast<uint32_t>(container_data_blk_addr));
    put_u32(reloc_tbl_bytes, brtd_block_size);
    put_s32(reloc_tbl_bytes, blk0_entry_count);
    put_s32(reloc_tbl_bytes, blk1_entry_count);

    for (const auto& re : reloc_entries) {
        put_u32(reloc_tbl_bytes, re.pos);
        put_u16(reloc_tbl_bytes, re.struct_count);
        reloc_tbl_bytes.push_back(re.offset_count);
        reloc_tbl_bytes.push_back(re.padding_count);
    }

    // Assemble fileData (TexContainer + 0x140 zero pad + infoPtrs + blocks)
    std::vector<uint8_t> file_data;
    file_data.insert(file_data.end(), target, target + 4);
    put_u32(file_data, total_tex);
    put_s64(file_data, 0x198);
    put_s64(file_data, container_data_blk_addr);
    put_s64(file_data, dict_pos);
    put_s64(file_data, 0x58);
    put_s64(file_data, 0);
    put_u32(file_data, 0);
    put_u32(file_data, 0);  // 4x

    file_data.insert(file_data.end(), 0x140, 0);
    for (uint32_t i = 0; i < total_tex; i++) {
        put_s64(file_data, textures[i].pos - 16);
    }
    file_data.insert(file_data.end(), str_tbl_hdr_bytes.begin(), str_tbl_hdr_bytes.end());
    file_data.insert(file_data.end(), str_tbl_bytes.begin(), str_tbl_bytes.end());
    file_data.insert(file_data.end(), tex_name_dict_bytes.begin(), tex_name_dict_bytes.end());
    file_data.insert(file_data.end(), info_blks.begin(), info_blks.end());
    file_data.insert(file_data.end(), data_blk.begin(), data_blk.end());
    file_data.insert(file_data.end(), reloc_tbl_bytes.begin(), reloc_tbl_bytes.end());

    uint32_t total_bntx_size = static_cast<uint32_t>(file_data.size() + 32);
    std::vector<uint8_t> final_bntx;
    final_bntx.reserve(total_bntx_size);
    final_bntx.insert(final_bntx.end(), {'B', 'N', 'T', 'X', 0, 0, 0, 0});
    put_u32(final_bntx, 0x40101);
    put_u16(final_bntx, 0xFEFF);
    final_bntx.push_back(align_shift);
    final_bntx.push_back(0x40);
    put_u32(final_bntx, new_file_name_addr);
    put_u16(final_bntx, 0);
    put_u16(final_bntx, new_first_blk_addr);
    put_u32(final_bntx, static_cast<uint32_t>(reloc_addr));
    put_u32(final_bntx, total_bntx_size);
    final_bntx.insert(final_bntx.end(), file_data.begin(), file_data.end());

    std::vector<uint8_t> new_sarc = build_sarc(final_bntx);
    return zstd_compress(new_sarc.data(), new_sarc.size(), output_arc_zst, 3, err);
}

bool list_sarc_bntx_textures(const std::vector<uint8_t>& input_arc_zst,
                             std::vector<std::string>& out_texture_names,
                             std::string* err) {
    out_texture_names.clear();
    std::vector<uint8_t> arc_data;
    if (!zstd_decompress(input_arc_zst.data(), input_arc_zst.size(), arc_data, err)) return false;
    if (arc_data.size() < 0x20 || std::memcmp(arc_data.data(), "SARC", 4) != 0) return false;
    uint32_t data_offset = u32_le(arc_data.data() + 0x0C);
    if (data_offset >= arc_data.size()) return false;

    const uint8_t* bntx = arc_data.data() + data_offset;
    uint16_t first_blk_addr = u16_le(bntx + 22);
    uint32_t tex_count = u32_le(bntx + 36);
    int64_t info_ptrs_addr = s64_le(bntx + 40);

    int64_t str_tbl_pos = static_cast<int64_t>(first_blk_addr) + 16;
    uint32_t str_count = u32_le(bntx + static_cast<size_t>(str_tbl_pos));
    std::map<int64_t, std::string> pos_to_str;
    int64_t cur = str_tbl_pos + 8;
    for (uint32_t i = 0; i < str_count; i++) {
        uint16_t slen = u16_le(bntx + static_cast<size_t>(cur));
        pos_to_str[cur] = std::string(reinterpret_cast<const char*>(bntx + static_cast<size_t>(cur + 2)), slen);
        cur += slen + 3;
        cur = ((cur - 1) | 1) + 1;
    }

    for (uint32_t i = 0; i < tex_count; i++) {
        int64_t tpos = s64_le(bntx + static_cast<size_t>(info_ptrs_addr + 8 * i));
        int64_t name_addr = s64_le(bntx + static_cast<size_t>(tpos + 16 + 80));
        auto it = pos_to_str.find(name_addr);
        if (it != pos_to_str.end()) {
            out_texture_names.push_back(it->second);
        }
    }
    return true;
}

}  // namespace fefw_foundation
