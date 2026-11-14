// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — In-Memory MSBT (.msbt / .msbt.zst) Label & Text Patcher
#include "fefw_foundation/msbt_patcher.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <zstd.h>

namespace fefw_foundation {
namespace {

uint32_t read_u32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

void append_u32(std::vector<uint8_t>& buf, uint32_t v) {
    uint8_t b[4];
    std::memcpy(b, &v, 4);
    buf.insert(buf.end(), b, b + 4);
}

bool is_zstd_frame(const uint8_t* d, size_t len) {
    if (len < 4) return false;
    return d[0] == 0x28 && d[1] == 0xB5 && d[2] == 0x2F && d[3] == 0xFD;
}

std::vector<uint8_t> utf8_to_utf16le_bytes(const std::string& utf8, bool append_null) {
    std::vector<uint8_t> out;
    out.reserve(utf8.size() * 2 + 2);
    size_t i = 0;
    while (i < utf8.size()) {
        uint32_t cp = 0;
        uint8_t c = static_cast<uint8_t>(utf8[i]);
        if (c < 0x80) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            cp = ((c & 0x1F) << 6) | (static_cast<uint8_t>(utf8[i + 1]) & 0x3F);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
            cp = ((c & 0x0F) << 12) |
                 ((static_cast<uint8_t>(utf8[i + 1]) & 0x3F) << 6) |
                 (static_cast<uint8_t>(utf8[i + 2]) & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < utf8.size()) {
            cp = ((c & 0x07) << 18) |
                 ((static_cast<uint8_t>(utf8[i + 1]) & 0x3F) << 12) |
                 ((static_cast<uint8_t>(utf8[i + 2]) & 0x3F) << 6) |
                 (static_cast<uint8_t>(utf8[i + 3]) & 0x3F);
            i += 4;
        } else {
            cp = c;
            i += 1;
        }
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<uint8_t>(cp & 0xFF));
            out.push_back(static_cast<uint8_t>((cp >> 8) & 0xFF));
        } else {
            cp -= 0x10000;
            uint16_t hi = static_cast<uint16_t>(0xD800 + ((cp >> 10) & 0x3FF));
            uint16_t lo = static_cast<uint16_t>(0xDC00 + (cp & 0x3FF));
            out.push_back(static_cast<uint8_t>(hi & 0xFF));
            out.push_back(static_cast<uint8_t>((hi >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>(lo & 0xFF));
            out.push_back(static_cast<uint8_t>((lo >> 8) & 0xFF));
        }
    }
    if (append_null) {
        out.push_back(0);
        out.push_back(0);
    }
    return out;
}

std::string utf16le_to_utf8_str(const uint8_t* d, size_t len) {
    std::string out;
    size_t i = 0;
    while (i + 1 < len) {
        uint16_t u = static_cast<uint16_t>(d[i]) | (static_cast<uint16_t>(d[i + 1]) << 8);
        if (u == 0) break;
        if (u == 0x000E) {
            // Skip MSBT control tag in UTF-16LE: 0x000E + group(2) + type(2) + arg_len(2) + args
            if (i + 8 > len) break;
            uint16_t arg_len = static_cast<uint16_t>(d[i + 6]) | (static_cast<uint16_t>(d[i + 7]) << 8);
            i += 8 + arg_len;
            continue;
        }
        uint32_t cp = u;
        i += 2;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < len) {
            uint16_t lo = static_cast<uint16_t>(d[i]) | (static_cast<uint16_t>(d[i + 1]) << 8);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + (((u - 0xD800) << 10) | (lo - 0xDC00));
                i += 2;
            }
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

uint32_t msbt_hash_label(const std::string& label, uint32_t num_groups) {
    uint32_t h = 0;
    for (unsigned char c : label) {
        h = h * 0x492u + c;
    }
    return h % num_groups;
}

int find_section(const std::vector<uint8_t>& data, const char magic[4]) {
    size_t pos = 0x20;
    while (pos + 16 <= data.size()) {
        if (std::memcmp(data.data() + pos, magic, 4) == 0) {
            return static_cast<int>(pos);
        }
        uint32_t sec_sz = read_u32(data.data() + pos + 4);
        size_t aligned = 16 + ((static_cast<size_t>(sec_sz) + 15) & ~size_t{15});
        pos += aligned;
    }
    return -1;
}

}  // namespace

bool zstd_decompress(const uint8_t* src, size_t src_len, std::vector<uint8_t>& out_data, std::string* err) {
    unsigned long long content_size = ZSTD_getFrameContentSize(src, src_len);
    if (content_size != ZSTD_CONTENTSIZE_UNKNOWN && content_size != ZSTD_CONTENTSIZE_ERROR) {
        out_data.resize(static_cast<size_t>(content_size));
        size_t ret = ZSTD_decompress(out_data.data(), out_data.size(), src, src_len);
        if (ZSTD_isError(ret)) {
            if (err) *err = std::string("ZSTD_decompress error: ") + ZSTD_getErrorName(ret);
            return false;
        }
        out_data.resize(ret);
        return true;
    }

    // Streaming fallback when content size is not in frame header
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    if (!dctx) return false;
    out_data.clear();
    std::vector<uint8_t> chunk(64 * 1024);
    ZSTD_inBuffer in_buf{src, src_len, 0};
    while (in_buf.pos < in_buf.size) {
        ZSTD_outBuffer out_buf{chunk.data(), chunk.size(), 0};
        size_t ret = ZSTD_decompressStream(dctx, &out_buf, &in_buf);
        if (ZSTD_isError(ret)) {
            if (err) *err = std::string("ZSTD_decompressStream error: ") + ZSTD_getErrorName(ret);
            ZSTD_freeDCtx(dctx);
            return false;
        }
        out_data.insert(out_data.end(), chunk.begin(), chunk.begin() + static_cast<ptrdiff_t>(out_buf.pos));
    }
    ZSTD_freeDCtx(dctx);
    return true;
}

bool zstd_compress(const uint8_t* src, size_t src_len, std::vector<uint8_t>& out_data, int level, std::string* err) {
    size_t bound = ZSTD_compressBound(src_len);
    out_data.resize(bound);
    size_t ret = ZSTD_compress(out_data.data(), out_data.size(), src, src_len, level);
    if (ZSTD_isError(ret)) {
        if (err) *err = std::string("ZSTD_compress error: ") + ZSTD_getErrorName(ret);
        return false;
    }
    out_data.resize(ret);
    return true;
}

// Rebuilds an ATR1 section so it holds one record per text entry after `added` new labels were appended.
// Returns false (leaving `out_sec` empty) when the section does not have the expected parallel layout.
bool rebuild_atr1(const std::vector<uint8_t>& raw, int atr_pos, uint32_t old_strings,
                  const std::vector<std::string>& new_labels,
                  const std::unordered_map<std::string, uint32_t>& label_to_idx,
                  const std::map<std::string, MsbtAtrSpec>* specs, std::vector<uint8_t>& out_sec) {
    uint32_t sec_size = read_u32(raw.data() + atr_pos + 4);
    const uint8_t* body = raw.data() + atr_pos + 16;
    if (sec_size < 8 || static_cast<size_t>(atr_pos) + 16 + sec_size > raw.size()) return false;
    uint32_t count = read_u32(body);
    uint32_t rsz = read_u32(body + 4);
    if (rsz < 4 || count != old_strings || 8ull + static_cast<uint64_t>(count) * rsz > sec_size) return false;
    const uint32_t pool_start = 8 + count * rsz;

    // string-offset fields: byte positions whose u32 lands inside the pool for every record
    std::vector<uint32_t> str_fields;
    for (uint32_t p = 0; p + 4 <= rsz;) {
        bool all = count > 0;
        for (uint32_t i = 0; i < count && all; ++i) {
            uint32_t v = read_u32(body + 8 + i * rsz + p);
            all = v >= pool_start && v < sec_size;
        }
        if (all) {
            str_fields.push_back(p);
            p += 4;
        } else {
            p += 1;
        }
    }

    const uint32_t added = static_cast<uint32_t>(new_labels.size());
    const uint32_t shift = added * rsz;
    std::vector<uint8_t> records(body + 8, body + 8 + count * rsz);
    std::vector<uint8_t> pool(body + pool_start, body + sec_size);
    auto put = [](uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); };
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t f : str_fields) {
            uint8_t* p = records.data() + i * rsz + f;
            put(p, read_u32(p) + shift);
        }
    }
    const uint32_t new_pool_start = pool_start + shift;
    auto add_string = [&](const std::string& s) {
        uint32_t off = new_pool_start + static_cast<uint32_t>(pool.size());
        pool.insert(pool.end(), s.begin(), s.end());
        pool.push_back(0);
        return off;
    };
    const uint32_t empty_off = add_string("");

    // new labels are appended in text-index order
    std::vector<std::pair<uint32_t, std::string>> ordered;
    for (const auto& l : new_labels) ordered.push_back({label_to_idx.at(l), l});
    std::sort(ordered.begin(), ordered.end());
    for (const auto& [idx, label] : ordered) {
        (void)idx;
        const MsbtAtrSpec* spec = nullptr;
        if (specs) {
            auto it = specs->find(label);
            if (it != specs->end()) spec = &it->second;
        }
        std::vector<uint8_t> rec(rsz, 0);
        uint32_t src = 0;
        bool blank = true;
        if (spec && !spec->template_label.empty()) {
            auto it = label_to_idx.find(spec->template_label);
            if (it != label_to_idx.end() && it->second < count) {
                src = it->second;
                blank = false;
            }
        }
        if (count > 0) std::memcpy(rec.data(), records.data() + src * rsz, rsz);
        if (blank) {
            for (uint32_t f : str_fields) put(rec.data() + f, empty_off);
        }
        if (spec) {
            for (const auto& [f, s] : spec->string_overrides) {
                if (f >= 0 && static_cast<uint32_t>(f) + 4 <= rsz) put(rec.data() + f, add_string(s));
            }
        }
        records.insert(records.end(), rec.begin(), rec.end());
    }

    std::vector<uint8_t> nb;
    append_u32(nb, count + added);
    append_u32(nb, rsz);
    nb.insert(nb.end(), records.begin(), records.end());
    nb.insert(nb.end(), pool.begin(), pool.end());
    uint32_t unpadded = static_cast<uint32_t>(nb.size());
    nb.insert(nb.end(), (16 - (nb.size() % 16)) % 16, 0xAB);
    out_sec.assign({'A', 'T', 'R', '1'});
    append_u32(out_sec, unpadded);
    append_u32(out_sec, 0);
    append_u32(out_sec, 0);
    out_sec.insert(out_sec.end(), nb.begin(), nb.end());
    return true;
}

bool patch_msbt_archive(const std::vector<uint8_t>& input_bytes,
                        const std::map<std::string, std::string>& label_to_utf8_text,
                        std::vector<uint8_t>& output_bytes,
                        std::string* err,
                        const std::map<std::string, MsbtAtrSpec>* atr_specs) {
    if (label_to_utf8_text.empty()) {
        output_bytes = input_bytes;
        return true;
    }

    bool compressed = is_zstd_frame(input_bytes.data(), input_bytes.size());
    std::vector<uint8_t> raw;
    if (compressed) {
        if (!zstd_decompress(input_bytes.data(), input_bytes.size(), raw, err)) return false;
    } else {
        raw = input_bytes;
    }

    if (raw.size() < 0x20 || std::memcmp(raw.data(), "MsgStdBn", 8) != 0) {
        if (err) *err = "Invalid MSBT header (missing MsgStdBn)";
        return false;
    }

    uint8_t encoding_type = raw[0x0C];  // 0 = UTF-8, 1 = UTF-16LE (0x08 BOM, 0x0A reserved)
    int lbl1_pos = find_section(raw, "LBL1");
    int txt2_pos = find_section(raw, "TXT2");
    if (lbl1_pos < 0 || txt2_pos < 0) {
        if (err) *err = "Missing LBL1 or TXT2 section in MSBT";
        return false;
    }

    uint32_t num_groups = read_u32(raw.data() + lbl1_pos + 16);
    std::vector<std::vector<std::pair<std::string, uint32_t>>> buckets(num_groups);
    std::unordered_map<std::string, uint32_t> label_to_idx;

    for (uint32_t g = 0; g < num_groups; g++) {
        uint32_t count = read_u32(raw.data() + lbl1_pos + 20 + g * 8);
        uint32_t off = read_u32(raw.data() + lbl1_pos + 24 + g * 8);
        size_t cur = static_cast<size_t>(lbl1_pos + 16 + off);
        for (uint32_t k = 0; k < count && cur < raw.size(); k++) {
            uint8_t slen = raw[cur++];
            std::string name(reinterpret_cast<const char*>(raw.data() + cur), slen);
            cur += slen;
            uint32_t idx = read_u32(raw.data() + cur);
            cur += 4;
            label_to_idx[name] = idx;
            buckets[g].push_back({name, idx});
        }
    }

    uint32_t txt2_size = read_u32(raw.data() + txt2_pos + 4);
    uint32_t num_strings = read_u32(raw.data() + txt2_pos + 16);
    std::vector<uint32_t> offsets(num_strings);
    for (uint32_t i = 0; i < num_strings; i++) {
        offsets[i] = read_u32(raw.data() + txt2_pos + 20 + i * 4);
    }

    std::vector<std::vector<uint8_t>> raw_strings(num_strings);
    for (uint32_t i = 0; i < num_strings; i++) {
        size_t start = static_cast<size_t>(txt2_pos + 16 + offsets[i]);
        size_t end = (i + 1 < num_strings)
                         ? static_cast<size_t>(txt2_pos + 16 + offsets[i + 1])
                         : static_cast<size_t>(txt2_pos + 16 + txt2_size);
        if (start <= end && end <= raw.size()) {
            raw_strings[i].assign(raw.begin() + static_cast<ptrdiff_t>(start),
                                  raw.begin() + static_cast<ptrdiff_t>(end));
        }
    }

    bool has_new_labels = false;
    std::vector<std::string> new_labels;
    for (const auto& [label, utf8_text] : label_to_utf8_text) {
        std::vector<uint8_t> encoded;
        if (encoding_type == 1) {
            encoded = utf8_to_utf16le_bytes(utf8_text, true);
        } else {
            encoded.assign(utf8_text.begin(), utf8_text.end());
            encoded.push_back(0);
        }

        auto it = label_to_idx.find(label);
        if (it != label_to_idx.end() && it->second < raw_strings.size()) {
            raw_strings[it->second] = std::move(encoded);
        } else {
            uint32_t new_idx = static_cast<uint32_t>(raw_strings.size());
            raw_strings.push_back(std::move(encoded));
            uint32_t b_idx = msbt_hash_label(label, num_groups);
            buckets[b_idx].push_back({label, new_idx});
            label_to_idx[label] = new_idx;
            has_new_labels = true;
            new_labels.push_back(label);
        }
    }

    uint32_t lbl1_size = read_u32(raw.data() + lbl1_pos + 4);
    size_t aligned_lbl1_size = 16 + ((static_cast<size_t>(lbl1_size) + 15) & ~size_t{15});
    size_t lbl1_end = static_cast<size_t>(lbl1_pos) + aligned_lbl1_size;

    size_t aligned_txt2_size = 16 + ((static_cast<size_t>(txt2_size) + 15) & ~size_t{15});
    size_t txt2_end = static_cast<size_t>(txt2_pos) + aligned_txt2_size;

    // 1. Rebuild LBL1 if new labels were added
    std::vector<uint8_t> lbl1_sec;
    if (has_new_labels) {
        std::vector<uint8_t> body;
        append_u32(body, num_groups);
        uint32_t cur_off = 4 + num_groups * 8;
        std::vector<uint8_t> bucket_data;
        for (uint32_t g = 0; g < num_groups; g++) {
            const auto& items = buckets[g];
            uint32_t count = static_cast<uint32_t>(items.size());
            uint32_t off = count > 0 ? cur_off : (4 + num_groups * 8);
            append_u32(body, count);
            append_u32(body, off);
            for (const auto& [name, idx] : items) {
                bucket_data.push_back(static_cast<uint8_t>(name.size()));
                bucket_data.insert(bucket_data.end(), name.begin(), name.end());
                append_u32(bucket_data, idx);
                cur_off += 1 + static_cast<uint32_t>(name.size()) + 4;
            }
        }
        body.insert(body.end(), bucket_data.begin(), bucket_data.end());
        uint32_t unpadded_len = static_cast<uint32_t>(body.size());
        size_t pad = (16 - (body.size() % 16)) % 16;
        body.insert(body.end(), pad, 0xAB);

        lbl1_sec.insert(lbl1_sec.end(), {'L', 'B', 'L', '1'});
        append_u32(lbl1_sec, unpadded_len);
        append_u32(lbl1_sec, 0);
        append_u32(lbl1_sec, 0);
        lbl1_sec.insert(lbl1_sec.end(), body.begin(), body.end());
    } else {
        lbl1_sec.assign(raw.begin() + lbl1_pos, raw.begin() + static_cast<ptrdiff_t>(lbl1_end));
    }

    // 2. Rebuild TXT2
    std::vector<uint8_t> txt2_body;
    uint32_t total_strings = static_cast<uint32_t>(raw_strings.size());
    append_u32(txt2_body, total_strings);
    uint32_t str_off = 4 + total_strings * 4;
    for (const auto& s : raw_strings) {
        append_u32(txt2_body, str_off);
        str_off += static_cast<uint32_t>(s.size());
    }
    for (const auto& s : raw_strings) {
        txt2_body.insert(txt2_body.end(), s.begin(), s.end());
    }
    uint32_t txt2_unpadded = static_cast<uint32_t>(txt2_body.size());
    size_t txt2_pad = (16 - (txt2_body.size() % 16)) % 16;
    txt2_body.insert(txt2_body.end(), txt2_pad, 0xAB);

    std::vector<uint8_t> txt2_sec;
    txt2_sec.insert(txt2_sec.end(), {'T', 'X', 'T', '2'});
    append_u32(txt2_sec, txt2_unpadded);
    append_u32(txt2_sec, 0);
    append_u32(txt2_sec, 0);
    txt2_sec.insert(txt2_sec.end(), txt2_body.begin(), txt2_body.end());

    // 2b. Keep ATR1 parallel to TXT2 for new labels (sits between LBL1 and TXT2 in FEFW files)
    std::vector<uint8_t> atr1_sec;
    int atr1_pos = find_section(raw, "ATR1");
    size_t atr1_end = 0;
    if (has_new_labels && atr1_pos > 0 && static_cast<size_t>(atr1_pos) >= lbl1_end &&
        atr1_pos < txt2_pos) {
        uint32_t atr_size = read_u32(raw.data() + atr1_pos + 4);
        atr1_end = static_cast<size_t>(atr1_pos) + 16 + ((static_cast<size_t>(atr_size) + 15) & ~size_t{15});
        if (!rebuild_atr1(raw, atr1_pos, num_strings, new_labels, label_to_idx, atr_specs, atr1_sec)) {
            atr1_sec.clear();
        }
    }

    // 3. Assemble full MSBT
    std::vector<uint8_t> rebuilt;
    rebuilt.insert(rebuilt.end(), raw.begin(), raw.begin() + lbl1_pos);
    rebuilt.insert(rebuilt.end(), lbl1_sec.begin(), lbl1_sec.end());
    if (!atr1_sec.empty()) {
        rebuilt.insert(rebuilt.end(), raw.begin() + static_cast<ptrdiff_t>(lbl1_end), raw.begin() + atr1_pos);
        rebuilt.insert(rebuilt.end(), atr1_sec.begin(), atr1_sec.end());
        if (atr1_end < static_cast<size_t>(txt2_pos)) {
            rebuilt.insert(rebuilt.end(), raw.begin() + static_cast<ptrdiff_t>(atr1_end), raw.begin() + txt2_pos);
        }
    } else if (lbl1_end < static_cast<size_t>(txt2_pos)) {
        rebuilt.insert(rebuilt.end(), raw.begin() + static_cast<ptrdiff_t>(lbl1_end), raw.begin() + txt2_pos);
    }
    rebuilt.insert(rebuilt.end(), txt2_sec.begin(), txt2_sec.end());
    if (txt2_end < raw.size()) {
        rebuilt.insert(rebuilt.end(), raw.begin() + static_cast<ptrdiff_t>(txt2_end), raw.end());
    }

    uint32_t total_msbt_size = static_cast<uint32_t>(rebuilt.size());
    std::memcpy(rebuilt.data() + 0x12, &total_msbt_size, 4);

    if (compressed) {
        return zstd_compress(rebuilt.data(), rebuilt.size(), output_bytes, 3, err);
    }
    output_bytes = std::move(rebuilt);
    return true;
}

bool read_msbt_messages(const std::vector<uint8_t>& input_bytes,
                        std::map<std::string, std::string>& out_messages,
                        std::string* err) {
    out_messages.clear();
    bool compressed = is_zstd_frame(input_bytes.data(), input_bytes.size());
    std::vector<uint8_t> raw;
    if (compressed) {
        if (!zstd_decompress(input_bytes.data(), input_bytes.size(), raw, err)) return false;
    } else {
        raw = input_bytes;
    }
    if (raw.size() < 0x20 || std::memcmp(raw.data(), "MsgStdBn", 8) != 0) {
        if (err) *err = "Invalid MSBT binary";
        return false;
    }
    uint8_t encoding_type = raw[0x0C];
    int lbl1_pos = find_section(raw, "LBL1");
    int txt2_pos = find_section(raw, "TXT2");
    if (lbl1_pos < 0 || txt2_pos < 0) return false;

    uint32_t num_groups = read_u32(raw.data() + lbl1_pos + 16);
    std::unordered_map<std::string, uint32_t> label_to_idx;
    for (uint32_t g = 0; g < num_groups; g++) {
        uint32_t count = read_u32(raw.data() + lbl1_pos + 20 + g * 8);
        uint32_t off = read_u32(raw.data() + lbl1_pos + 24 + g * 8);
        size_t cur = static_cast<size_t>(lbl1_pos + 16 + off);
        for (uint32_t k = 0; k < count && cur < raw.size(); k++) {
            uint8_t slen = raw[cur++];
            std::string name(reinterpret_cast<const char*>(raw.data() + cur), slen);
            cur += slen;
            uint32_t idx = read_u32(raw.data() + cur);
            cur += 4;
            label_to_idx[name] = idx;
        }
    }

    uint32_t txt2_size = read_u32(raw.data() + txt2_pos + 4);
    uint32_t num_strings = read_u32(raw.data() + txt2_pos + 16);
    std::vector<uint32_t> offsets(num_strings);
    for (uint32_t i = 0; i < num_strings; i++) {
        offsets[i] = read_u32(raw.data() + txt2_pos + 20 + i * 4);
    }

    for (const auto& [name, idx] : label_to_idx) {
        if (idx >= num_strings) continue;
        size_t start = static_cast<size_t>(txt2_pos + 16 + offsets[idx]);
        size_t end = (idx + 1 < num_strings)
                         ? static_cast<size_t>(txt2_pos + 16 + offsets[idx + 1])
                         : static_cast<size_t>(txt2_pos + 16 + txt2_size);
        if (start <= end && end <= raw.size()) {
            if (encoding_type == 1) {
                out_messages[name] = utf16le_to_utf8_str(raw.data() + start, end - start);
            } else {
                const void* z = std::memchr(raw.data() + start, 0, end - start);
                size_t slen = z ? static_cast<size_t>(static_cast<const uint8_t*>(z) - (raw.data() + start)) : (end - start);
                out_messages[name] = std::string(reinterpret_cast<const char*>(raw.data() + start), slen);
            }
        }
    }
    return true;
}

}  // namespace fefw_foundation
