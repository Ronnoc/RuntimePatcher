// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — GameData table (.bin) reader, writer and row editing. Format: docs/TABLE_FORMAT.md.
#include "fefw_foundation/game_table.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace fefw_foundation {
namespace {

// Pointers in a table file are stored relative to the end of the 32-byte file header.
constexpr int64_t kBody = 32;
constexpr int kRowHeaderSize = 32;
constexpr int kDescriptorSize = 32;

// Scalar kinds a field type name can denote.
enum class Scalar { kU8, kS8, kBool, kU16, kS16, kU32, kS32, kS64, kF32, kFlags8, kFlags16, kFlags32, kColor, kRef, kText, kOther };

Scalar scalar_of(const std::string& t) {
    static const std::unordered_map<std::string, Scalar> kNames = {
        {"u8", Scalar::kU8},       {"s8", Scalar::kS8},       {"bool", Scalar::kBool},   {"u16", Scalar::kU16},
        {"s16", Scalar::kS16},     {"u32", Scalar::kU32},     {"s32", Scalar::kS32},     {"int", Scalar::kS32},
        {"s64", Scalar::kS64},     {"f32", Scalar::kF32},     {"float", Scalar::kF32},   {"b8", Scalar::kFlags8},
        {"b16", Scalar::kFlags16}, {"b32", Scalar::kFlags32}, {"color", Scalar::kColor}, {"ref", Scalar::kRef},
        {"string", Scalar::kText}, {"data", Scalar::kText},
    };
    auto it = kNames.find(t);
    return it == kNames.end() ? Scalar::kOther : it->second;
}

bool known_scalar(const std::string& t) { return scalar_of(t) != Scalar::kOther; }

// Bytes one array element of this type occupies.
int element_bytes(const std::string& t) {
    switch (scalar_of(t)) {
        case Scalar::kU8: case Scalar::kS8: case Scalar::kBool: case Scalar::kFlags8: return 1;
        case Scalar::kU16: case Scalar::kS16: case Scalar::kFlags16: return 2;
        case Scalar::kU32: case Scalar::kS32: case Scalar::kF32: case Scalar::kFlags32: case Scalar::kColor: return 4;
        default: return t == "data" ? 16 : 8;
    }
}

std::string strip_array_suffix(const std::string& t) {
    if (t.size() >= 3 && t.compare(t.size() - 3, 3, "[,]") == 0) return t.substr(0, t.size() - 3);
    if (t.size() >= 2 && t.compare(t.size() - 2, 2, "[]") == 0) return t.substr(0, t.size() - 2);
    return t;
}

// ------------------------------------------------------------------------------------------------
// Reading
// ------------------------------------------------------------------------------------------------
class Bytes {
public:
    Bytes(const uint8_t* data, size_t size) : data_(data), size_(static_cast<int64_t>(size)) {}

    int64_t size() const { return size_; }
    bool has(int64_t at, int64_t n) const { return at >= 0 && n >= 0 && at + n <= size_; }

    template <typename T>
    T at(int64_t pos) const {
        T v{};
        std::memcpy(&v, data_ + pos, sizeof(T));
        return v;
    }
    uint8_t u8(int64_t p) const { return data_[p]; }
    uint16_t u16(int64_t p) const { return at<uint16_t>(p); }
    int32_t s32(int64_t p) const { return at<int32_t>(p); }
    uint64_t u64(int64_t p) const { return at<uint64_t>(p); }

    // NUL-terminated string behind a pointer; false (and `out` untouched) for null / out-of-range pointers.
    bool text(uint64_t ptr, std::string& out) const {
        if (ptr == 0 || ptr >= static_cast<uint64_t>(size_)) return false;
        int64_t start = kBody + static_cast<int64_t>(ptr);
        if (start >= size_) return false;
        const void* nul = std::memchr(data_ + start, 0, static_cast<size_t>(size_ - start));
        int64_t end = nul ? static_cast<const uint8_t*>(nul) - data_ : size_;
        out.assign(reinterpret_cast<const char*>(data_ + start), static_cast<size_t>(end - start));
        return true;
    }

private:
    const uint8_t* data_;
    int64_t size_;
};

// What the sheet root says about a sheet (when the file has one).
struct SheetShape {
    int field_count = 0;
    bool grouped = false;
    int entries = 0;
    int record_size = 0;
};

class SheetReader {
public:
    explicit SheetReader(const Bytes& b) : b_(b) {}

    TableValue scalar(int64_t pos, const std::string& type, int size) const {
        if (!b_.has(pos, 8)) return TableValue::make_null();
        switch (scalar_of(type)) {
            case Scalar::kU8: return TableValue::make_int(b_.u8(pos));
            case Scalar::kS8: return TableValue::make_int(b_.at<int8_t>(pos));
            case Scalar::kBool: return TableValue::make_int(b_.u8(pos) != 0 ? 1 : 0);
            case Scalar::kU16: return TableValue::make_int(b_.u16(pos));
            case Scalar::kS16: return TableValue::make_int(b_.at<int16_t>(pos));
            case Scalar::kU32: return TableValue::make_int(b_.at<uint32_t>(pos));
            case Scalar::kS32: return TableValue::make_int(b_.s32(pos));
            case Scalar::kS64: return TableValue::make_int(b_.at<int64_t>(pos));
            case Scalar::kF32: return TableValue::make_float(static_cast<double>(b_.at<float>(pos)));
            case Scalar::kFlags8: return TableValue::make_flags(b_.u8(pos), 8);
            case Scalar::kFlags16: return TableValue::make_flags(b_.u16(pos), 16);
            case Scalar::kFlags32: return TableValue::make_flags(b_.at<uint32_t>(pos), 32);
            case Scalar::kColor: {
                uint32_t c = b_.at<uint32_t>(pos);
                return TableValue::make_rgba(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, c >> 24);
            }
            case Scalar::kRef: {
                // Shown as the key of the record it points at.
                uint64_t ptr = b_.u64(pos);
                if (ptr == 0 || ptr >= static_cast<uint64_t>(b_.size())) return TableValue::make_null();
                int64_t target = kBody + static_cast<int64_t>(ptr);
                std::string key;
                if (b_.has(target, 16) && b_.text(b_.u64(target + 8), key)) return TableValue::make_string(std::move(key));
                return TableValue::make_null();
            }
            case Scalar::kText: {
                std::string s;
                if (b_.text(b_.u64(pos), s)) return TableValue::make_string(std::move(s));
                return TableValue::make_null();
            }
            case Scalar::kOther:
                if (size == 1) return TableValue::make_int(b_.u8(pos));
                if (size == 2) return TableValue::make_int(b_.u16(pos));
                if (size == 4) return TableValue::make_int(b_.at<uint32_t>(pos));
                return TableValue::make_null();
        }
        return TableValue::make_null();
    }

    // Array slot: u16 count, u16 element size, 4 bytes padding, pointer to the elements.
    TableArray1D list(int64_t slot, const std::string& elem) const {
        TableArray1D out;
        if (!b_.has(slot, 16)) return out;
        int count = b_.u16(slot), stride = b_.u16(slot + 2);
        uint64_t ptr = b_.u64(slot + 8);
        if (count == 0 || ptr == 0) return out;
        int64_t first = kBody + static_cast<int32_t>(ptr);
        if (!b_.has(first, static_cast<int64_t>(count) * stride)) return out;
        out.reserve(count);
        for (int i = 0; i < count; ++i) out.push_back(scalar(first + static_cast<int64_t>(i) * stride, elem, stride));
        return out;
    }

    TableArray2D list_of_lists(int64_t slot, const std::string& elem) const {
        TableArray2D out;
        if (!b_.has(slot, 16)) return out;
        int count = b_.u16(slot);
        uint64_t ptr = b_.u64(slot + 8);
        if (count == 0 || ptr == 0) return out;
        int64_t first = kBody + static_cast<int32_t>(ptr);
        if (!b_.has(first, static_cast<int64_t>(count) * 16)) return out;
        out.reserve(count);
        for (int i = 0; i < count; ++i) out.push_back(list(first + static_cast<int64_t>(i) * 16, elem));
        return out;
    }

    TableValue field(int64_t record, const TableField& f) const {
        int64_t slot = record + f.offset;
        if (f.is_2d()) return TableValue::make_array2d(list_of_lists(slot, f.element_type()));
        if (f.is_array()) return TableValue::make_array1d(list(slot, f.element_type()));
        return scalar(slot, f.element_type(), f.size);
    }

    TableRow row(int64_t record, std::string key, std::string group, int index, int sub,
                 const std::vector<TableField>& fields, bool with_header) const {
        TableRow r;
        r.key = std::move(key);
        r.group = std::move(group);
        r.index = index;
        r.sub = sub;
        if (with_header) {
            r.header_index = b_.u16(record + 16);
            r.header_count = b_.u16(record + 18);
            r.line = b_.s32(record + 20);
        } else {
            r.header_index = r.header_count = r.line = 0;
        }
        r.values.reserve(fields.size());
        for (const auto& f : fields) r.values.push_back(field(record, f));
        return r;
    }

    // Heuristic for sheets without a root: a run of group headers (index 0, 1, ...) whose first non-empty group
    // points at a record carrying the same key.
    bool looks_grouped(int64_t pos) const {
        for (int n = 0; n < 16 && b_.has(pos, 48); ++n, pos += 48) {
            if (b_.u64(pos) != 0 || b_.u16(pos + 16) != n) return n > 0;
            if (b_.u16(pos + 18) == 0) continue;
            uint64_t ptr = b_.u64(pos + 32);
            if (ptr == 0 || ptr >= static_cast<uint64_t>(b_.size())) return false;
            int64_t target = kBody + static_cast<int32_t>(ptr);
            if (b_.has(target, 32) && target != pos && b_.u64(target) == 0) return b_.u64(target + 8) == b_.u64(pos + 8);
            return false;
        }
        return false;
    }

    // A plausible field descriptor at `pos` (used to find sheets by scanning).
    bool looks_like_descriptor(int64_t pos) const {
        if (pos < kBody || !b_.has(pos, kDescriptorSize)) return false;
        int size = b_.s32(pos + 24), offset = b_.s32(pos + 28);
        if (size <= 0 || size > 4096 || offset < 32 || offset > 65536) return false;
        std::string type, name;
        if (!b_.text(b_.u64(pos + 8), type) || !known_scalar(strip_array_suffix(type))) return false;
        return b_.text(b_.u64(pos), name) && !name.empty();
    }

    // Reads the sheet whose field descriptors start at `start`. `shape` comes from the sheet root, if any.
    GameTable sheet(int64_t start, const SheetShape* shape) const {
        GameTable t;
        int64_t pos = start;
        for (; b_.has(pos, kDescriptorSize); pos += kDescriptorSize) {
            if (shape && static_cast<int>(t.fields.size()) >= shape->field_count) break;
            uint64_t name_ptr = b_.u64(pos), type_ptr = b_.u64(pos + 8);
            int size = b_.s32(pos + 24);
            if (name_ptr == 0 || type_ptr == 0 || size <= 0 || size > 4096) break;
            TableField f;
            if (!b_.text(name_ptr, f.name) || !b_.text(type_ptr, f.type_name) || f.type_name.empty()) break;
            f.size = size;
            f.offset = b_.s32(pos + 28);
            f.min_val = b_.at<float>(pos + 16);
            f.max_val = b_.at<float>(pos + 20);
            t.fields.push_back(std::move(f));
        }

        int extent = 32, lowest = 32;
        if (!t.fields.empty()) {
            extent = 0;
            lowest = t.fields[0].offset;
            for (const auto& f : t.fields) {
                extent = std::max(extent, f.offset + f.size);
                lowest = std::min(lowest, f.offset);
            }
            extent = (extent + 7) & ~7;
        }
        const bool headerless = !t.fields.empty() && lowest < 32;
        t.row_size = shape ? shape->record_size : extent;
        t.grouped = shape ? shape->grouped : looks_grouped(pos);
        const int64_t stride = !t.grouped ? t.row_size : (headerless ? 40 : 48);

        int64_t end = pos;
        std::string run_key;
        int sub = 0;
        for (int entry = 0; b_.has(pos, stride); ++entry, pos += stride) {
            if (shape ? entry >= shape->entries : (b_.u64(pos) != 0 || b_.u16(pos + 16) != entry)) break;
            end = pos + stride;
            std::string key;
            b_.text(b_.u64(pos + 8), key);
            if (!t.grouped) {
                if (!key.empty() || t.rows.empty()) {
                    run_key = key;
                    sub = 0;
                } else {
                    ++sub;
                }
                t.rows.push_back(row(pos, key, run_key, static_cast<int>(t.rows.size()), sub, t.fields, true));
                continue;
            }
            const int count = b_.u16(pos + 18);
            t.groups.push_back(TableGroup{key, b_.u16(pos + 16), count, b_.s32(pos + 20)});
            const int64_t first = kBody + static_cast<int64_t>(b_.u64(pos + 32));
            for (int r = 0; r < count; ++r) {
                const int64_t rec = first + static_cast<int64_t>(r) * t.row_size;
                if (first <= 32 || rec + t.row_size > b_.size()) break;
                std::string row_key = headerless ? key : std::string();
                if (!headerless) b_.text(b_.u64(rec + 8), row_key);
                t.rows.push_back(row(rec, row_key, key, static_cast<int>(t.rows.size()), r, t.fields, !headerless));
            }
        }
        t.offset = static_cast<int>(start);
        t.end_offset = static_cast<int>(end);
        return t;
    }

    // Visits the key index entries: fn(record position) returns true to stop.
    template <typename Fn>
    void each_index_entry(Fn fn) const {
        const int64_t first = kBody + b_.s32(4) + static_cast<int64_t>(b_.s32(8)) * 8;
        const int count = b_.s32(12);
        for (int i = 0; i < count && b_.has(first + static_cast<int64_t>(i) * 8, 8); ++i) {
            if (fn(kBody + b_.s32(first + static_cast<int64_t>(i) * 8))) return;
        }
    }

    // Sheets reachable through sheet roots in the key index, in file order.
    std::vector<GameTable> sheets_from_roots() const {
        std::vector<GameTable> out;
        each_index_entry([&](int64_t root) {
            if (root < 32 || !b_.has(root, 48) || b_.u64(root) != 0) return false;
            const uint64_t info_ptr = b_.u64(root + 32), rows_ptr = b_.u64(root + 40);
            if (info_ptr == 0 || info_ptr + 64 > static_cast<uint64_t>(b_.size())) return false;
            const int64_t info = kBody + static_cast<int32_t>(info_ptr);
            const int fields = b_.s32(info + 20), record_size = b_.s32(info + 16);
            const uint64_t fields_ptr = b_.u64(info + 24);
            if (fields < 0 || fields > 4096 || record_size <= 0 || fields_ptr >= static_cast<uint64_t>(b_.size()) ||
                static_cast<int64_t>(fields_ptr) + static_cast<int64_t>(fields) * kDescriptorSize != static_cast<int64_t>(rows_ptr)) {
                return false;
            }
            const uint64_t version = b_.u64(info);
            SheetShape shape{fields, (version >> 32) == 2, b_.u16(root + 18), record_size};
            GameTable t = sheet(kBody + static_cast<int32_t>(fields_ptr), &shape);
            t.version = version;
            b_.text(b_.u64(info + 8), t.name);
            out.push_back(std::move(t));
            return false;
        });
        std::stable_sort(out.begin(), out.end(), [](const GameTable& a, const GameTable& b) { return a.offset < b.offset; });
        return out;
    }

    // Name and version for a sheet found by scanning: the root whose rows pointer matches the sheet.
    void name_from_root(GameTable& t) const {
        const uint64_t rows_ptr = static_cast<uint64_t>(t.offset + static_cast<int>(t.fields.size()) * kDescriptorSize - kBody);
        each_index_entry([&](int64_t root) {
            if (root < 32 || !b_.has(root, 48) || b_.u64(root + 40) != rows_ptr || b_.u64(root) != 0) return false;
            const int64_t info = kBody + static_cast<int32_t>(b_.u64(root + 32));
            if (info < 32 || !b_.has(info, 32)) return false;
            t.version = b_.u64(info);
            b_.text(b_.u64(info + 8), t.name);
            return true;
        });
    }

private:
    const Bytes& b_;
};

}  // namespace

std::string TableValue::to_string() const {
    switch (kind) {
        case Kind::kNull: return "";
        case Kind::kInt: return std::to_string(int_val);
        case Kind::kFloat: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.9g", float_val);
            return buf;
        }
        case Kind::kString: return str_val;
        case Kind::kFlags: return std::to_string(flags_val.bits);
        case Kind::kRgba: {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", rgba_val.r, rgba_val.g, rgba_val.b, rgba_val.a);
            return buf;
        }
        case Kind::kArray1D: {
            std::string s = "[";
            for (size_t i = 0; i < arr_1d.size(); i++) {
                if (i > 0) s += ", ";
                s += arr_1d[i].to_string();
            }
            s += "]";
            return s;
        }
        case Kind::kArray2D: {
            std::string s = "[";
            for (size_t i = 0; i < arr_2d.size(); i++) {
                if (i > 0) s += ", ";
                s += "[";
                for (size_t j = 0; j < arr_2d[i].size(); j++) {
                    if (j > 0) s += ", ";
                    s += arr_2d[i][j].to_string();
                }
                s += "]";
            }
            s += "]";
            return s;
        }
    }
    return "";
}

bool TableField::is_2d() const {
    return type_name.size() >= 3 && type_name.compare(type_name.size() - 3, 3, "[,]") == 0;
}

bool TableField::is_array() const {
    if (is_2d()) return true;
    return type_name.size() >= 2 && type_name.compare(type_name.size() - 2, 2, "[]") == 0;
}

std::string TableField::element_type() const {
    if (is_2d()) return type_name.substr(0, type_name.size() - 3);
    if (is_array()) return type_name.substr(0, type_name.size() - 2);
    return type_name;
}

bool TableField::is_reference() const {
    std::string e = element_type();
    return e == "data" || e == "ref";
}

bool GameTable::headerless() const {
    if (fields.empty()) return false;
    int min_off = fields[0].offset;
    for (const auto& f : fields) min_off = std::min(min_off, f.offset);
    return min_off < 32;
}

TableRow* GameTable::find_row(const std::string& key) {
    for (auto& r : rows) {
        if (r.key == key) return &r;
    }
    return nullptr;
}

const TableRow* GameTable::find_row(const std::string& key) const {
    for (const auto& r : rows) {
        if (r.key == key) return &r;
    }
    return nullptr;
}

int GameTable::find_row_index(const std::string& key) const {
    for (size_t i = 0; i < rows.size(); i++) {
        if (rows[i].key == key) return static_cast<int>(i);
    }
    return -1;
}

void GameTable::renumber_inline() {
    int sub = 0;
    for (size_t i = 0; i < rows.size(); i++) {
        auto& r = rows[i];
        sub = (r.key.empty() && i != 0) ? (sub + 1) : 0;
        r.index = static_cast<int>(i);
        r.header_index = static_cast<int>(i);
        r.sub = sub;
    }
}

void GameTable::renumber_group(size_t g) {
    if (g >= groups.size()) return;
    const std::string& gkey = groups[g].key;
    int sub = 0;
    for (size_t i = 0; i < rows.size(); i++) {
        rows[i].index = static_cast<int>(i);
        if (rows[i].group == gkey) {
            rows[i].sub = sub;
            rows[i].header_index = sub;
            sub++;
        }
    }
    groups[g].count = sub;
}

TableRow& GameTable::upsert_keyed_row(const std::string& key, const std::string& clone_from_key) {
    if (auto* existing = find_row(key)) {
        return *existing;
    }
    TableRow new_row;
    new_row.key = key;
    new_row.group = key;
    new_row.index = static_cast<int>(rows.size());
    new_row.header_index = static_cast<int>(rows.size());
    new_row.header_count = 1;
    new_row.line = rows.empty() ? 0 : rows.back().line;

    const TableRow* proto = !clone_from_key.empty() ? find_row(clone_from_key) : nullptr;
    if (proto) {
        new_row.values = proto->values;
        new_row.line = proto->line;
    } else {
        new_row.values.resize(fields.size(), TableValue::make_null());
    }
    rows.push_back(std::move(new_row));
    renumber_inline();
    return rows.back();
}

TableRow& GameTable::append_row(const std::string& key, const TableRow* proto) {
    TableRow new_row;
    size_t pos = rows.size();
    if (proto) {
        new_row = *proto;  // copy before inserting: proto may point into `rows`
        // keep the row next to its template (same section); never behind a trailing '>' section marker
        if (proto >= rows.data() && proto < rows.data() + rows.size()) {
            pos = static_cast<size_t>(proto - rows.data()) + 1;
            // skip the template's keyless continuation rows so we do not split a keyed group
            while (!key.empty() && pos < rows.size() && rows[pos].key.empty()) ++pos;
        }
    } else {
        new_row.values.resize(fields.size(), TableValue::make_null());
        new_row.line = rows.empty() ? 0 : rows.back().line;
        if (!rows.empty() && !rows.back().key.empty() && rows.back().key[0] == '>') pos = rows.size() - 1;
    }
    new_row.key = key;
    new_row.group = key;
    new_row.header_count = 1;
    new_row.sub = 0;
    rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(pos), std::move(new_row));
    renumber_inline();
    return rows[pos];
}

TableRow* GameTable::insert_group_row(const std::string& group_key, const TableRow* proto) {
    if (!grouped) return nullptr;
    size_t g = groups.size();
    for (size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].key == group_key) {
            g = i;
            break;
        }
    }
    if (g == groups.size()) return nullptr;
    // rows are stored group after group; find the insertion point (end of group g)
    size_t pos = 0;
    for (size_t i = 0; i <= g; ++i) pos += static_cast<size_t>(groups[i].count);
    if (pos > rows.size()) return nullptr;
    TableRow nr;
    if (proto) {
        nr = *proto;
    } else {
        nr.values.resize(fields.size(), TableValue::make_null());
    }
    nr.key = group_key;
    nr.group = group_key;
    nr.header_count = 1;
    if (pos > 0) nr.line = rows[pos - 1].line;
    rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(pos), std::move(nr));
    renumber_group(g);
    return &rows[pos];
}

bool GameTable::set_group_rows(const std::string& group_key, std::vector<TableRow> new_rows) {
    if (!grouped) return false;
    size_t g = groups.size();
    for (size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].key == group_key) {
            g = i;
            break;
        }
    }
    // rows are stored group after group: start of group g = sum of the previous counts
    size_t pos = 0;
    if (g == groups.size()) {
        TableGroup ng;
        ng.key = group_key;
        ng.index = groups.empty() ? 0 : groups.back().index + 1;
        ng.line = groups.empty() ? 0 : groups.back().line;
        groups.push_back(ng);
        pos = rows.size();
    } else {
        for (size_t i = 0; i < g; ++i) pos += static_cast<size_t>(groups[i].count);
        size_t n = static_cast<size_t>(groups[g].count);
        if (pos + n > rows.size()) return false;
        rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(pos), rows.begin() + static_cast<std::ptrdiff_t>(pos + n));
    }
    for (auto& r : new_rows) {
        r.key = group_key;
        r.group = group_key;
        r.header_count = 1;
    }
    rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(pos), new_rows.begin(), new_rows.end());
    renumber_group(g);
    return true;
}

const TableRow* GameTable::find_row_by_clause(size_t col_idx, const std::vector<std::string>& clause) const {
    for (const auto& r : rows) {
        if (col_idx >= r.values.size() || r.values[col_idx].kind != TableValue::Kind::kArray2D) continue;
        for (const auto& inner : r.values[col_idx].arr_2d) {
            if (inner.size() != clause.size()) continue;
            bool same = true;
            for (size_t i = 0; i < inner.size() && same; ++i) {
                same = inner[i].kind == TableValue::Kind::kString && inner[i].str_val == clause[i];
            }
            if (same) return &r;
        }
    }
    return nullptr;
}

int GameTable::clone_2d_array_clauses(size_t col_idx, const std::map<std::string, std::string>& replacements) {
    if (col_idx >= fields.size() || !fields[col_idx].is_2d()) return 0;
    std::set<std::string> source_keys;
    std::set<std::string> target_keys;
    for (const auto& [src, dst] : replacements) {
        if (!src.empty() && src[0] != '!') source_keys.insert(src);
        if (!dst.empty() && dst[0] != '!') target_keys.insert(dst);
    }

    int patched_rows = 0;
    for (auto& r : rows) {
        if (col_idx >= r.values.size() || r.values[col_idx].kind != TableValue::Kind::kArray2D) continue;
        auto& outer = r.values[col_idx].arr_2d;
        TableArray2D new_clauses;

        for (const auto& inner : outer) {
            bool has_src = false;
            bool has_dst = false;
            for (const auto& v : inner) {
                if (v.kind == TableValue::Kind::kString) {
                    if (source_keys.count(v.str_val)) has_src = true;
                    if (target_keys.count(v.str_val)) has_dst = true;
                }
            }
            if (has_src && !has_dst) {
                TableArray1D cloned = inner;
                for (auto& v : cloned) {
                    if (v.kind == TableValue::Kind::kString) {
                        auto it = replacements.find(v.str_val);
                        if (it != replacements.end()) {
                            v.str_val = it->second;
                        }
                    }
                }
                new_clauses.push_back(std::move(cloned));
            }
        }
        if (!new_clauses.empty()) {
            for (auto& nc : new_clauses) {
                outer.push_back(std::move(nc));
            }
            patched_rows++;
        }
    }
    return patched_rows;
}

// ------------------------------------------------------------------------------------------------
// Reading: entry points
// ------------------------------------------------------------------------------------------------
bool GameTable::is_table(const uint8_t* data, size_t size) {
    if (size < 64) return false;
    uint32_t declared = 0;
    std::memcpy(&declared, data, 4);
    return declared == static_cast<uint32_t>(size);
}

bool GameTable::read_sheets(const std::vector<uint8_t>& data, std::vector<GameTable>& out_sheets, bool include_empty,
                            std::string* err) {
    out_sheets.clear();
    if (!is_table(data.data(), data.size())) {
        if (err) *err = "Not a valid GameData table binary.";
        return false;
    }
    const Bytes bytes(data.data(), data.size());
    const SheetReader reader(bytes);

    std::vector<GameTable> sheets = reader.sheets_from_roots();
    if (!sheets.empty()) {
        for (size_t i = 0; i < sheets.size(); ++i) {
            if (include_empty || i == 0 || !sheets[i].rows.empty()) out_sheets.push_back(std::move(sheets[i]));
        }
        return true;
    }

    // No usable sheet roots: the first sheet starts right after the header, later ones are found by scanning.
    GameTable first = reader.sheet(kBody, nullptr);
    reader.name_from_root(first);
    int64_t pos = (first.end_offset + 7) & ~7;
    out_sheets.push_back(std::move(first));
    for (; bytes.has(pos, 64); pos += 8) {
        if (!reader.looks_like_descriptor(pos) || !reader.looks_like_descriptor(pos + 32) ||
            reader.looks_like_descriptor(pos - 32)) {
            continue;
        }
        GameTable next = reader.sheet(pos, nullptr);
        if (next.rows.empty()) continue;
        reader.name_from_root(next);
        pos = ((next.end_offset + 7) & ~7) - 8;
        out_sheets.push_back(std::move(next));
    }
    return true;
}

// ------------------------------------------------------------------------------------------------
// Writing
// ------------------------------------------------------------------------------------------------
namespace {

// Builds a table file: sheet bodies first, then the relocation list, key index and string pool.
class Emitter {
public:
    Emitter() {
        out_.reserve(256 * 1024);
        pad(static_cast<int>(kBody));  // file header, filled in by finish()
    }

    int here() const { return static_cast<int>(out_.size()); }
    void pad(int n) { if (n > 0) out_.resize(out_.size() + static_cast<size_t>(n), 0); }
    void align8() { pad((8 - here() % 8) % 8); }

    template <typename T>
    void put_at(int pos, T v) { std::memcpy(out_.data() + pos, &v, sizeof(T)); }
    template <typename T>
    void put(T v) {
        int pos = here();
        pad(sizeof(T));
        put_at(pos, v);
    }

    // Pointer slot at `slot` -> `target` (recorded for the relocation list).
    void link(int slot, int target) {
        put_at<uint64_t>(slot, static_cast<uint64_t>(target - kBody));
        pointers_.push_back(slot);
    }
    void put_link(int target) {
        int slot = here();
        pad(8);
        link(slot, target);
    }

    // String slot at `slot`; resolved into the string pool by finish(). nullptr leaves a null pointer.
    void text_at(int slot, const std::string* s) { if (s) texts_.push_back({slot, *s}); }
    void put_text(const std::string* s) {
        int slot = here();
        pad(8);
        text_at(slot, s);
    }

    // Registers a record under its key: the first record with a key goes into the key index, the last one is
    // what `ref` fields with that key point at.
    void key(int record, const std::string& k) {
        if (indexed_.insert(k).second) index_.push_back({record, k});
        if (!k.empty()) records_[k] = record;
    }

    void ref_at(int slot, const std::string& target_key) { refs_.push_back({slot, target_key}); }

    void row_header(const std::string& k, int index, int count, int line) {
        put<uint64_t>(0);
        put_text(k.empty() ? nullptr : &k);
        put<uint16_t>(static_cast<uint16_t>(index));
        put<uint16_t>(static_cast<uint16_t>(count));
        put<uint32_t>(static_cast<uint32_t>(line));
        put<uint64_t>(0);
    }

    std::vector<uint8_t> finish() {
        for (const auto& [slot, k] : refs_) {
            auto it = records_.find(k);
            if (it != records_.end()) link(slot, it->second);
        }
        align8();

        // String pool: index keys first, then the remaining strings in address order of their first use.
        std::unordered_map<std::string, int> pool_at;
        std::vector<uint8_t> pool;
        auto intern = [&](const std::string& s) {
            if (pool_at.emplace(s, static_cast<int>(pool.size())).second) {
                pool.insert(pool.end(), s.begin(), s.end());
                pool.push_back(0);
            }
        };
        for (const auto& e : index_) intern(e.key);
        std::sort(texts_.begin(), texts_.end(), [](const TextSlot& a, const TextSlot& b) { return a.slot < b.slot; });

        // Relocations: plain pointers in address order, then string slots grouped by string (first use order).
        std::vector<int> relocs = pointers_;
        std::sort(relocs.begin(), relocs.end());
        std::vector<std::string> order;
        std::unordered_map<std::string, std::vector<int>> slots_of;
        for (const auto& t : texts_) {
            intern(t.text);
            auto [it, fresh] = slots_of.try_emplace(t.text);
            if (fresh) order.push_back(t.text);
            it->second.push_back(t.slot);
        }
        for (const auto& s : order) relocs.insert(relocs.end(), slots_of[s].begin(), slots_of[s].end());

        const int reloc_list = here();
        for (int slot : relocs) put<uint64_t>(static_cast<uint64_t>(slot - kBody));
        for (const auto& e : index_) {
            put<uint32_t>(static_cast<uint32_t>(e.record - kBody));
            put<uint32_t>(static_cast<uint32_t>(pool_at[e.key]));
        }
        const int pool_start = here();
        out_.insert(out_.end(), pool.begin(), pool.end());
        for (const auto& t : texts_) put_at<uint64_t>(t.slot, static_cast<uint64_t>(pool_start + pool_at[t.text] - kBody));

        put_at<uint32_t>(0, static_cast<uint32_t>(out_.size()));
        put_at<uint32_t>(4, static_cast<uint32_t>(reloc_list - kBody));
        put_at<uint32_t>(8, static_cast<uint32_t>(relocs.size()));
        put_at<uint32_t>(12, static_cast<uint32_t>(index_.size()));
        put_at<uint32_t>(20, 1u);
        return std::move(out_);
    }

private:
    struct TextSlot { int slot; std::string text; };
    struct IndexEntry { int record; std::string key; };

    std::vector<uint8_t> out_;
    std::vector<int> pointers_;
    std::vector<TextSlot> texts_;
    std::vector<IndexEntry> index_;
    std::unordered_set<std::string> indexed_;
    std::unordered_map<std::string, int> records_;
    std::vector<std::pair<int, std::string>> refs_;
};

// Canonical text of an array's contents, used to store identical arrays once per sheet.
std::string content_id(const TableArray1D& items) {
    std::string id;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) id += '\x02';
        if (items[i].is_null()) id += '\x03'; else id += items[i].to_string();
    }
    return id;
}

std::string content_id(const TableArray2D& items) {
    std::string id;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) id += '\x02';
        id += '[' + content_id(items[i]) + ']';
    }
    return id;
}

uint32_t parse_color_text(const std::string& text, bool& ok) {
    // "#RRGGBB" or "#RRGGBBAA" (as table dumps print colors) -> R | G<<8 | B<<16 | A<<24
    std::string hex = (!text.empty() && text[0] == '#') ? text.substr(1) : text;
    ok = hex.size() == 6 || hex.size() == 8;
    if (!ok) return 0;
    uint32_t v = static_cast<uint32_t>(std::strtoul(hex.c_str(), nullptr, 16));
    if (hex.size() == 6) v = (v << 8) | 0xFF;
    return ((v >> 24) & 0xFF) | (((v >> 16) & 0xFF) << 8) | (((v >> 8) & 0xFF) << 16) | ((v & 0xFF) << 24);
}

class SheetWriter {
public:
    SheetWriter(Emitter& e, const GameTable& t) : e_(e), t_(t), headerless_(t.headerless()) {}

    void write() {
        const int descriptors = e_.here();
        for (const auto& f : t_.fields) {
            e_.put_text(&f.name);
            e_.put_text(&f.type_name);
            e_.put<float>(f.min_val);
            e_.put<float>(f.max_val);
            e_.put<uint32_t>(static_cast<uint32_t>(f.size));
            e_.put<uint32_t>(static_cast<uint32_t>(f.offset));
        }
        const int rows = e_.here();
        if (t_.grouped) {
            write_groups();
        } else {
            for (const auto& r : t_.rows) record(r, !r.key.empty());
        }
        write_arrays();

        e_.align8();
        const int info = e_.here();
        e_.put<uint64_t>(t_.version);
        e_.put_text(&t_.name);
        e_.put<uint32_t>(static_cast<uint32_t>(t_.row_size));
        e_.put<uint32_t>(static_cast<uint32_t>(t_.fields.size()));
        e_.put_link(descriptors);
        e_.key(e_.here(), t_.name);
        e_.row_header(t_.name, 0, static_cast<int>(t_.grouped ? t_.groups.size() : t_.rows.size()), 0);
        e_.put_link(info);
        e_.put_link(rows);
    }

private:
    struct PendingArray { int slot; const TableField* field; const TableValue* value; };
    struct PendingList { int slot; const TableField* field; const TableArray1D* items; };

    void write_groups() {
        std::vector<int> headers;
        for (const auto& g : t_.groups) {
            headers.push_back(e_.here());
            if (!g.key.empty()) e_.key(e_.here(), g.key);
            e_.row_header(g.key, g.index, g.count, g.line);
            e_.put<uint64_t>(0);                 // -> first record of the group
            if (!headerless_) e_.put<uint64_t>(0);
        }
        size_t next = 0;
        for (size_t i = 0; i < t_.groups.size(); ++i) {
            e_.link(headers[i] + 32, e_.here());
            for (int j = 0; j < t_.groups[i].count && next < t_.rows.size(); ++j) {
                const TableRow& r = t_.rows[next++];
                record(r, !r.key.empty() && !headerless_);
            }
        }
    }

    void record(const TableRow& r, bool indexed) {
        const int at = e_.here();
        if (indexed) e_.key(at, r.key);
        if (headerless_) {
            e_.pad(t_.row_size);
        } else {
            e_.row_header(r.key, r.header_index, r.header_count, r.line);
            e_.pad(t_.row_size - kRowHeaderSize);
        }
        static const TableValue kNull;
        for (size_t i = 0; i < t_.fields.size(); ++i) {
            const TableField& f = t_.fields[i];
            const TableValue& v = i < r.values.size() ? r.values[i] : kNull;
            if (f.is_array()) arrays_.push_back({at + f.offset, &f, &v});
            else scalar(at + f.offset, f.element_type(), f.size, v);
        }
    }

    void scalar(int at, const std::string& type, int size, const TableValue& v) {
        using K = TableValue::Kind;
        switch (scalar_of(type)) {
            case Scalar::kRef:
                if (v.kind == K::kString && !v.str_val.empty()) e_.ref_at(at, v.str_val);
                return;
            case Scalar::kText:
                if (v.kind == K::kString) e_.text_at(at, &v.str_val);
                return;
            case Scalar::kF32:
                e_.put_at<float>(at, v.kind == K::kFloat ? static_cast<float>(v.float_val)
                                     : v.kind == K::kInt ? static_cast<float>(v.int_val) : 0.0f);
                return;
            case Scalar::kColor:
                if (v.kind == K::kRgba) {
                    e_.put_at<uint32_t>(at, v.rgba_val.r | (v.rgba_val.g << 8) | (v.rgba_val.b << 16) |
                                                (static_cast<uint32_t>(v.rgba_val.a) << 24));
                } else if (v.kind == K::kString) {
                    bool ok = false;
                    uint32_t c = parse_color_text(v.str_val, ok);
                    if (ok) e_.put_at<uint32_t>(at, c);
                } else if (v.kind == K::kInt) {
                    e_.put_at<uint32_t>(at, static_cast<uint32_t>(v.int_val));
                }
                return;
            default:
                break;
        }
        const uint64_t n = v.kind == K::kInt ? static_cast<uint64_t>(v.int_val)
                         : v.kind == K::kFlags ? v.flags_val.bits : 0;
        int width;
        switch (scalar_of(type)) {
            case Scalar::kU8: case Scalar::kS8: case Scalar::kBool: case Scalar::kFlags8: width = 1; break;
            case Scalar::kU16: case Scalar::kS16: case Scalar::kFlags16: width = 2; break;
            case Scalar::kS64: width = 8; break;
            default: width = size >= 4 ? 4 : size; break;
        }
        if (width == 1) e_.put_at<uint8_t>(at, static_cast<uint8_t>(n));
        else if (width == 2) e_.put_at<uint16_t>(at, static_cast<uint16_t>(n));
        else if (width == 8) e_.put_at<uint64_t>(at, n);
        else e_.put_at<uint32_t>(at, static_cast<uint32_t>(n));
    }

    // Array slot header; returns false when the slot needs no new payload (empty, or shared with an earlier one).
    bool array_slot(int slot, int count, int elem_bytes, const std::string& id, int& payload) {
        e_.put_at<uint16_t>(slot, static_cast<uint16_t>(count));
        e_.put_at<uint16_t>(slot + 2, static_cast<uint16_t>(elem_bytes));
        if (count == 0) return false;
        auto it = payloads_.find(id);
        if (it != payloads_.end()) {
            e_.link(slot + 8, it->second);
            return false;
        }
        e_.align8();
        payload = e_.here();
        payloads_[id] = payload;
        e_.link(slot + 8, payload);
        e_.pad(count * elem_bytes);
        return true;
    }

    void list(int slot, const TableField& f, const TableArray1D& items) {
        const std::string elem = f.element_type();
        const int bytes = element_bytes(elem);
        int payload = 0;
        if (!array_slot(slot, static_cast<int>(items.size()), bytes, "1" + elem + "\x01" + content_id(items), payload)) return;
        for (size_t i = 0; i < items.size(); ++i) scalar(payload + static_cast<int>(i) * bytes, elem, bytes, items[i]);
    }

    // Payload order: outer arrays of 2D fields, then 1D fields, then the inner arrays of the 2D fields.
    void write_arrays() {
        static const TableArray1D kEmpty1;
        static const TableArray2D kEmpty2;
        std::vector<PendingList> inner;
        for (const auto& a : arrays_) {
            if (!a.field->is_2d()) continue;
            const TableArray2D& items = a.value->kind == TableValue::Kind::kArray2D ? a.value->arr_2d : kEmpty2;
            const std::string id = "2" + a.field->element_type() + "\x01" + content_id(items);
            int payload = 0;
            if (array_slot(a.slot, static_cast<int>(items.size()), 16, id, payload)) {
                for (size_t k = 0; k < items.size(); ++k) inner.push_back({payload + static_cast<int>(k) * 16, a.field, &items[k]});
            }
        }
        for (const auto& a : arrays_) {
            if (a.field->is_2d()) continue;
            list(a.slot, *a.field, a.value->kind == TableValue::Kind::kArray1D ? a.value->arr_1d : kEmpty1);
        }
        for (const auto& l : inner) list(l.slot, *l.field, *l.items);
    }

    Emitter& e_;
    const GameTable& t_;
    const bool headerless_;
    std::vector<PendingArray> arrays_;
    std::unordered_map<std::string, int> payloads_;
};

}  // namespace

std::vector<uint8_t> GameTable::write_sheets(const std::vector<GameTable>& sheets) {
    Emitter e;
    for (const auto& t : sheets) SheetWriter(e, t).write();
    return e.finish();
}

}  // namespace fefw_foundation
