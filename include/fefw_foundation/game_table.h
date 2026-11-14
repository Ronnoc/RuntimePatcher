// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — Universal GameData (.bin) Table Parser, Serializer & Patch Engine
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace fefw_foundation {

struct FlagsValue {
    uint32_t bits = 0;
    int width = 8;
    bool operator==(const FlagsValue& o) const = default;
};

struct RgbaValue {
    uint8_t r = 0, g = 0, b = 0, a = 255;
    bool operator==(const RgbaValue& o) const = default;
};

struct TableValue;
using TableArray1D = std::vector<TableValue>;
using TableArray2D = std::vector<TableArray1D>;

struct TableValue {
    enum class Kind : uint8_t {
        kNull,
        kInt,
        kFloat,
        kString,
        kFlags,
        kRgba,
        kArray1D,
        kArray2D
    };

    Kind kind = Kind::kNull;
    int64_t int_val = 0;
    double float_val = 0.0;
    std::string str_val;
    FlagsValue flags_val{};
    RgbaValue rgba_val{};
    TableArray1D arr_1d;
    TableArray2D arr_2d;

    static TableValue make_null() { return TableValue{}; }
    static TableValue make_int(int64_t v) {
        TableValue tv;
        tv.kind = Kind::kInt;
        tv.int_val = v;
        return tv;
    }
    static TableValue make_float(double v) {
        TableValue tv;
        tv.kind = Kind::kFloat;
        tv.float_val = v;
        return tv;
    }
    static TableValue make_string(std::string v) {
        TableValue tv;
        tv.kind = Kind::kString;
        tv.str_val = std::move(v);
        return tv;
    }
    static TableValue make_flags(uint32_t bits, int width) {
        TableValue tv;
        tv.kind = Kind::kFlags;
        tv.flags_val = {bits, width};
        return tv;
    }
    static TableValue make_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        TableValue tv;
        tv.kind = Kind::kRgba;
        tv.rgba_val = {r, g, b, a};
        return tv;
    }
    static TableValue make_array1d(TableArray1D v) {
        TableValue tv;
        tv.kind = Kind::kArray1D;
        tv.arr_1d = std::move(v);
        return tv;
    }
    static TableValue make_array2d(TableArray2D v) {
        TableValue tv;
        tv.kind = Kind::kArray2D;
        tv.arr_2d = std::move(v);
        return tv;
    }

    bool is_null() const { return kind == Kind::kNull; }
    std::string to_string() const;
};

struct TableField {
    std::string name;
    std::string type_name;
    int size = 0;
    int offset = 0;
    float min_val = 0.0f;
    float max_val = 0.0f;

    bool is_2d() const;
    bool is_array() const;
    std::string element_type() const;
    bool is_reference() const;
};

struct TableGroup {
    std::string key;
    int index = 0;
    int count = 0;
    int line = 0;
};

struct TableRow {
    std::string key;
    int index = 0;
    std::string group;
    int sub = 0;
    std::vector<TableValue> values;
    int header_index = 0;
    int header_count = 1;
    int line = 0;
};

class GameTable {
public:
    std::vector<TableField> fields;
    std::vector<TableRow> rows;
    std::vector<TableGroup> groups;
    int row_size = 32;
    bool grouped = false;
    int offset = 32;
    int end_offset = 32;
    std::string name;
    uint64_t version = 0;

    bool headerless() const;

    // Row lookup & manipulation
    TableRow* find_row(const std::string& key);
    const TableRow* find_row(const std::string& key) const;
    int find_row_index(const std::string& key) const;

    // Appends or updates a keyed row in an inline (non-grouped) sheet.
    // Always appends new keys at the end of `rows` so existing row indices [0..N-1] never shift!
    TableRow& upsert_keyed_row(const std::string& key, const std::string& clone_from_key);

    // Appends a row at the end (keys may be empty/duplicated, e.g. AssetTable condition rows). `proto` (optional)
    // supplies the initial values; otherwise every column starts null.
    TableRow& append_row(const std::string& key, const TableRow* proto);

    // Grouped sheets (HubDispos ...): inserts a row after the last row of group `group_key` and bumps the group's
    // count. `proto` (optional) supplies initial values. Returns nullptr when the group does not exist.
    TableRow* insert_group_row(const std::string& group_key, const TableRow* proto);

    // Grouped sheets: replaces every row of group `group_key` with `new_rows` (rows' key/group are set here),
    // appending the group at the end when it does not exist yet. Returns false on an inline sheet.
    bool set_group_rows(const std::string& group_key, std::vector<TableRow> new_rows);

    // First row whose 2D column `col_idx` contains an inner clause exactly equal to `clause`.
    const TableRow* find_row_by_clause(size_t col_idx, const std::vector<std::string>& clause) const;

    // Clones 2D array clauses in `col_idx` across all rows where any element matches a source key in `replacements`
    int clone_2d_array_clauses(size_t col_idx, const std::map<std::string, std::string>& replacements);

    void renumber_inline();
    void renumber_group(size_t g);

    // Binary Deserialization & Serialization
    static bool is_table(const uint8_t* data, size_t size);
    static bool read_sheets(const std::vector<uint8_t>& data, std::vector<GameTable>& out_sheets, bool include_empty = true, std::string* err = nullptr);
    static std::vector<uint8_t> write_sheets(const std::vector<GameTable>& sheets);
};

}  // namespace fefw_foundation
