// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — Universal Virtual RomFS Table Overlay Engine
#include "fefw_foundation/vfs_overlay.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#ifndef FEFW_FOUNDATION_NO_STD_FILESYSTEM
#include <filesystem>
#include <fstream>
#endif
#include <map>
#include <set>

namespace fefw_foundation {
namespace {

// ============================================================================
// Host path helpers ('/'-joined UTF-8 strings, see HostFs)
// ============================================================================
std::string path_join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/' || a.back() == '\\') return a + b;
    return a + "/" + b;
}

std::string path_parent(const std::string& p) {
    size_t end = p.size();
    while (end > 0 && (p[end - 1] == '/' || p[end - 1] == '\\')) --end;
    size_t pos = p.find_last_of("/\\", end == 0 ? 0 : end - 1);
    if (end == 0 || pos == std::string::npos) return {};
    if (pos > 0 && p[pos - 1] == ':') return p.substr(0, pos + 1);  // "C:/x" -> "C:/", "Contents:/x" -> "Contents:/"
    if (pos == 0) return p.substr(0, 1);
    return p.substr(0, pos);
}

bool path_has_ext(const std::string& p, const char* ext) {
    size_t n = std::strlen(ext);
    return p.size() >= n && p.compare(p.size() - n, n, ext) == 0;
}

// ============================================================================
// Minimal, Zero-Dependency JSON Parser (RFC 8259)
// ============================================================================
struct JsonVal {
    enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
    Type type = Type::kNull;
    bool b_val = false;
    double num_val = 0.0;
    int64_t int_val = 0;
    std::string str_val;
    std::vector<JsonVal> arr_val;
    std::map<std::string, JsonVal> obj_val;

    bool is_null() const { return type == Type::kNull; }
    bool is_obj() const { return type == Type::kObject; }
    bool is_arr() const { return type == Type::kArray; }
    bool is_str() const { return type == Type::kString; }
    bool is_num() const { return type == Type::kNumber; }
    bool is_bool() const { return type == Type::kBool; }

    const JsonVal* get(const std::string& k) const {
        if (type != Type::kObject) return nullptr;
        auto it = obj_val.find(k);
        return (it != obj_val.end()) ? &it->second : nullptr;
    }

    std::string str_or(const std::string& k, const std::string& def = "") const {
        const auto* v = get(k);
        return (v && v->type == Type::kString) ? v->str_val : def;
    }

    int64_t int_or(const std::string& k, int64_t def = 0) const {
        const auto* v = get(k);
        return (v && v->type == Type::kNumber) ? v->int_val : def;
    }

    bool bool_or(const std::string& k, bool def = false) const {
        const auto* v = get(k);
        return (v && v->type == Type::kBool) ? v->b_val : def;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    bool parse(JsonVal& out) {
        skip_ws();
        return parse_val(out);
    }

private:
    const std::string& s_;
    size_t p_ = 0;

    void skip_ws() {
        while (p_ < s_.size()) {
            unsigned char c = static_cast<unsigned char>(s_[p_]);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                p_++;
            } else if (c == '/' && p_ + 1 < s_.size() && s_[p_ + 1] == '/') {
                p_ += 2;
                while (p_ < s_.size() && s_[p_] != '\n') p_++;
            } else {
                break;
            }
        }
    }

    bool parse_val(JsonVal& out) {
        skip_ws();
        if (p_ >= s_.size()) return false;
        char c = s_[p_];
        if (c == '{') return parse_obj(out);
        if (c == '[') return parse_arr(out);
        if (c == '"') {
            out.type = JsonVal::Type::kString;
            return parse_str(out.str_val);
        }
        if (c == 't' && s_.compare(p_, 4, "true") == 0) {
            p_ += 4;
            out.type = JsonVal::Type::kBool;
            out.b_val = true;
            return true;
        }
        if (c == 'f' && s_.compare(p_, 5, "false") == 0) {
            p_ += 5;
            out.type = JsonVal::Type::kBool;
            out.b_val = false;
            return true;
        }
        if (c == 'n' && s_.compare(p_, 4, "null") == 0) {
            p_ += 4;
            out.type = JsonVal::Type::kNull;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parse_num(out);
        }
        return false;
    }

    static void append_utf8(std::string& out, uint32_t cp) {
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

    bool parse_hex4(uint16_t& out) {
        if (p_ + 4 > s_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; i++) {
            char c = s_[p_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= (c - '0');
            else if (c >= 'a' && c <= 'f') out |= (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= (c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool parse_str(std::string& out) {
        out.clear();
        if (p_ >= s_.size() || s_[p_] != '"') return false;
        p_++;
        while (p_ < s_.size()) {
            char c = s_[p_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (p_ >= s_.size()) return false;
                char esc = s_[p_++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        uint16_t u1 = 0;
                        if (!parse_hex4(u1)) return false;
                        uint32_t cp = u1;
                        if (u1 >= 0xD800 && u1 <= 0xDBFF && p_ + 2 <= s_.size() && s_[p_] == '\\' && s_[p_ + 1] == 'u') {
                            p_ += 2;
                            uint16_t u2 = 0;
                            if (!parse_hex4(u2)) return false;
                            cp = 0x10000 + (((u1 - 0xD800) << 10) | (u2 - 0xDC00));
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default: out.push_back(esc); break;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool parse_num(JsonVal& out) {
        size_t start = p_;
        if (s_[p_] == '-') p_++;
        while (p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9') p_++;
        bool is_float = false;
        if (p_ < s_.size() && s_[p_] == '.') {
            is_float = true;
            p_++;
            while (p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9') p_++;
        }
        if (p_ < s_.size() && (s_[p_] == 'e' || s_[p_] == 'E')) {
            is_float = true;
            p_++;
            if (p_ < s_.size() && (s_[p_] == '+' || s_[p_] == '-')) p_++;
            while (p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9') p_++;
        }
        std::string sub = s_.substr(start, p_ - start);
        out.type = JsonVal::Type::kNumber;
        if (is_float) {
            out.num_val = std::strtod(sub.c_str(), nullptr);
            out.int_val = static_cast<int64_t>(out.num_val);
        } else {
            out.int_val = std::strtoll(sub.c_str(), nullptr, 10);
            out.num_val = static_cast<double>(out.int_val);
        }
        return true;
    }

    bool parse_arr(JsonVal& out) {
        out.type = JsonVal::Type::kArray;
        out.arr_val.clear();
        p_++;  // skip '['
        skip_ws();
        if (p_ < s_.size() && s_[p_] == ']') {
            p_++;
            return true;
        }
        while (p_ < s_.size()) {
            JsonVal elem;
            if (!parse_val(elem)) return false;
            out.arr_val.push_back(std::move(elem));
            skip_ws();
            if (p_ < s_.size() && s_[p_] == ',') {
                p_++;
                continue;
            }
            if (p_ < s_.size() && s_[p_] == ']') {
                p_++;
                return true;
            }
            return false;
        }
        return false;
    }

    bool parse_obj(JsonVal& out) {
        out.type = JsonVal::Type::kObject;
        out.obj_val.clear();
        p_++;  // skip '{'
        skip_ws();
        if (p_ < s_.size() && s_[p_] == '}') {
            p_++;
            return true;
        }
        while (p_ < s_.size()) {
            skip_ws();
            std::string key;
            if (!parse_str(key)) return false;
            skip_ws();
            if (p_ >= s_.size() || s_[p_] != ':') return false;
            p_++;
            JsonVal val;
            if (!parse_val(val)) return false;
            out.obj_val[std::move(key)] = std::move(val);
            skip_ws();
            if (p_ < s_.size() && s_[p_] == ',') {
                p_++;
                continue;
            }
            if (p_ < s_.size() && s_[p_] == '}') {
                p_++;
                return true;
            }
            return false;
        }
        return false;
    }
};

// ============================================================================
// High-Level Manifest & Generic Table Patch Applier
// ============================================================================
// Active HostFs for the current apply_vfs_overlays() call (set on entry, single-threaded use).
const HostFs* g_host_fs = nullptr;

bool read_host_file(const std::string& p, std::vector<uint8_t>& out) {
    out.clear();
    return g_host_fs && g_host_fs->read_file && g_host_fs->read_file(p, out);
}

TableValue json_to_table_value(const JsonVal& jv, const TableField& f) {
    if (jv.is_null()) return TableValue::make_null();
    if (f.is_2d() && jv.is_arr()) {
        TableArray2D arr2d;
        TableField elem_f = f;
        elem_f.type_name = f.element_type();
        for (const auto& row_j : jv.arr_val) {
            TableArray1D inner;
            if (row_j.is_arr()) {
                for (const auto& cell_j : row_j.arr_val) {
                    inner.push_back(json_to_table_value(cell_j, elem_f));
                }
            }
            arr2d.push_back(std::move(inner));
        }
        return TableValue::make_array2d(std::move(arr2d));
    }
    if (f.is_array() && jv.is_arr()) {
        TableArray1D arr1d;
        TableField elem_f = f;
        elem_f.type_name = f.element_type();
        for (const auto& cell_j : jv.arr_val) {
            arr1d.push_back(json_to_table_value(cell_j, elem_f));
        }
        return TableValue::make_array1d(std::move(arr1d));
    }

    std::string et = f.element_type();
    if (et == "b8" || et == "b16" || et == "b32") {
        int w = (et == "b8") ? 8 : ((et == "b16") ? 16 : 32);
        if (jv.is_num()) return TableValue::make_flags(static_cast<uint32_t>(jv.int_val), w);
        if (jv.is_str() && jv.str_val.rfind("0x", 0) == 0) {
            uint32_t bits = static_cast<uint32_t>(std::strtoul(jv.str_val.c_str() + 2, nullptr, 16));
            return TableValue::make_flags(bits, w);
        }
    }
    if (et == "f32" || et == "float") {
        if (jv.is_num()) return TableValue::make_float(jv.num_val);
    }
    if (et == "string" || et == "data" || et == "ref") {
        if (jv.is_str()) {
            if (jv.str_val.empty()) return TableValue::make_null();
            return TableValue::make_string(jv.str_val);
        }
        return TableValue::make_null();
    }
    if (jv.is_num()) return TableValue::make_int(jv.int_val);
    if (jv.is_bool()) return TableValue::make_int(jv.b_val ? 1 : 0);
    if (jv.is_str()) return TableValue::make_string(jv.str_val);
    return TableValue::make_null();
}

TableArray1D make_str_array(const JsonVal* arr_j) {
    TableArray1D res;
    if (arr_j && arr_j->is_arr()) {
        for (const auto& item : arr_j->arr_val) {
            if (item.is_str()) res.push_back(TableValue::make_string(item.str_val));
        }
    }
    return res;
}

void apply_class_manifest_to_job_table(GameTable& job_sheet, const JsonVal& manifest) {
    const auto* classes = manifest.get("classes");
    if (!classes || !classes->is_arr()) return;

    // 1. First pass: upsert all new class rows at the end of Job.bin so intra-mod `ref` targets exist
    for (const auto& cls : classes->arr_val) {
        if (!cls.is_obj()) continue;
        std::string key = cls.str_or("key");
        std::string clone_base = cls.str_or("clone_base", "JID_兵士");
        if (key.empty()) continue;

        TableRow& row = job_sheet.upsert_keyed_row(key, clone_base);
        if (row.values.size() < 134) continue;

        row.values[0] = TableValue::make_string(cls.str_or("name_id"));
        row.values[1] = TableValue::make_string(cls.str_or("help_id"));
        if (const auto* mv = cls.get("movement_type")) row.values[2] = TableValue::make_int(mv->int_val);
        int64_t rank = cls.int_or("rank", 2);
        row.values[3] = TableValue::make_int(rank);
        if (const auto* gen = cls.get("gender")) row.values[4] = TableValue::make_int(gen->int_val);
        row.values[5] = TableValue::make_int(0);

        // Col 7: Lower Class, Col 8: Upper Class, Col 9: Dismount Target Class
        std::string lower = cls.str_or("lower_class");
        std::string upper = cls.str_or("upper_class");
        std::string dismount = cls.str_or("dismount_target");
        row.values[7] = lower.empty() ? TableValue::make_null() : TableValue::make_string(lower);
        row.values[8] = upper.empty() ? TableValue::make_null() : TableValue::make_string(upper);
        row.values[9] = dismount.empty() ? TableValue::make_null() : TableValue::make_string(dismount);

        std::string mount_rid = cls.str_or("mount_rid");
        row.values[11] = mount_rid.empty() ? TableValue::make_null() : TableValue::make_string(mount_rid);
        row.values[12] = TableValue::make_int(1);

        row.values[13] = TableValue::make_array1d({});
        row.values[14] = TableValue::make_array1d(make_str_array(cls.get("innate_skills")));
        for (int c = 15; c <= 19; c++) row.values[c] = TableValue::make_array1d({});
        row.values[20] = TableValue::make_array1d(make_str_array(cls.get("attribute_skills")));
        row.values[21] = TableValue::make_array1d(make_str_array(cls.get("mastery_skills")));
        row.values[22] = TableValue::make_null();

        // Base Stats (Col 23..33)
        if (const auto* bs = cls.get("base_stats"); bs && bs->is_obj()) {
            const char* kStatKeys[11] = {"hp", "str", "mag", "dex", "spd", "lck", "def", "res", "cha", "vision", "mov"};
            for (int i = 0; i < 11; i++) {
                if (const auto* sv = bs->get(kStatKeys[i])) row.values[23 + i] = TableValue::make_int(sv->int_val);
            }
        }

        // Stat Modifiers (Col 34..44)
        if (const auto* sm = cls.get("stat_modifiers"); sm && sm->is_obj()) {
            const char* kStatKeys[11] = {"hp", "str", "mag", "dex", "spd", "lck", "def", "res", "cha", "vision", "mov"};
            for (int i = 0; i < 11; i++) {
                row.values[34 + i] = TableValue::make_int(sm->int_or(kStatKeys[i], 0));
            }
        }

        // Stat Caps (Col 45..55): always 99
        for (int c = 45; c <= 55; c++) row.values[c] = TableValue::make_int(99);

        // Enemy stat scaling columns (Col 56..66)
        if (const auto* es = cls.get("enemy_scalings"); es && es->is_arr()) {
            for (size_t i = 0; i < es->arr_val.size() && i < 11; i++) {
                row.values[56 + i] = TableValue::make_int(es->arr_val[i].int_val);
            }
        }

        // Growths % (Col 67..77)
        if (const auto* gr = cls.get("growths"); gr && gr->is_obj()) {
            const char* kGrowthKeys[9] = {"hp", "str", "mag", "dex", "spd", "lck", "def", "res", "cha"};
            for (int i = 0; i < 9; i++) {
                row.values[67 + i] = TableValue::make_int(gr->int_or(kGrowthKeys[i], 0));
            }
            row.values[76] = TableValue::make_int(0);
            row.values[77] = TableValue::make_int(0);
        }

        // Weapon EXP (Col 78..90)
        if (const auto* wexp = cls.get("weapon_exp"); wexp && wexp->is_obj()) {
            const char* kWepKeys[13] = {
                "sword", "lance", "axe", "bow", "brawling", "reason",
                "white_magic", "special", "infantry", "riding", "armored", "flying", "authority"
            };
            for (int i = 0; i < 13; i++) {
                if (const auto* wv = wexp->get(kWepKeys[i])) {
                    row.values[78 + i] = TableValue::make_int(wv->int_val);
                } else {
                    row.values[78 + i] = TableValue::make_int(-1);
                }
            }
        }

        // Mastery & Battle EXP (Col 91, 92)
        row.values[91] = TableValue::make_int(rank == 2 ? 60 : (rank == 3 ? 100 : 130));
        row.values[92] = TableValue::make_int(rank == 2 ? 15 : (rank == 3 ? 20 : 25));
        row.values[93] = TableValue::make_int(0);
        row.values[94] = TableValue::make_int(1);
        row.values[95] = TableValue::make_int(2);
        row.values[96] = TableValue::make_int(3);

        // Exam Requirements (Col 97..132)
        const auto* exam = cls.get("exam");
        if (exam && exam->is_obj()) {
            const char* kWepKeys[13] = {
                "sword", "lance", "axe", "bow", "brawling", "reason",
                "white_magic", "special", "infantry", "riding", "armored", "flying", "authority"
            };
            const auto* reqs = exam->get("requirements");
            for (int i = 0; i < 13; i++) {
                int64_t rv = (reqs && reqs->is_obj()) ? reqs->int_or(kWepKeys[i], 0) : 0;
                row.values[97 + i] = TableValue::make_int(rv);
            }
            if (rank == 2) {
                row.values[110] = TableValue::make_int(1);
                row.values[111] = TableValue::make_int(2);
                row.values[112] = TableValue::make_int(15);
            } else {
                row.values[110] = TableValue::make_int(0);
                row.values[111] = TableValue::make_int(1);
                row.values[112] = TableValue::make_int(15);
            }
            row.values[113] = TableValue::make_array1d({});
            row.values[114] = TableValue::make_array1d({});
            std::string seal = exam->str_or("seal");
            row.values[115] = seal.empty() ? TableValue::make_null() : TableValue::make_string(seal);
            row.values[117] = TableValue::make_int(rank == 2 ? 30 : 15);
            row.values[118] = TableValue::make_int(0);
            std::string wdisp = exam->str_or("weapon_display");
            row.values[120] = wdisp.empty() ? TableValue::make_null() : TableValue::make_string(wdisp);
            row.values[121] = TableValue::make_int(cls.int_or("system", 0));

            const char* kStatKeys[11] = {"hp", "str", "mag", "dex", "spd", "lck", "def", "res", "cha", "vision", "mov"};
            const auto* mins = exam->get("stat_minimums");
            for (int i = 0; i < 11; i++) {
                int64_t mv = (mins && mins->is_obj()) ? mins->int_or(kStatKeys[i], 0) : 0;
                row.values[122 + i] = TableValue::make_int(mv);
            }
        } else {
            // Dismounted / non-exam form
            for (int w = 97; w <= 109; w++) row.values[w] = TableValue::make_int(0);
            row.values[110] = TableValue::make_int(15);
            row.values[111] = TableValue::make_int(15);
            row.values[112] = TableValue::make_int(15);
            row.values[113] = TableValue::make_array1d({});
            row.values[114] = TableValue::make_array1d({});
            row.values[115] = TableValue::make_null();
            row.values[116] = TableValue::make_array1d({});
            row.values[117] = TableValue::make_int(0);
            row.values[118] = TableValue::make_int(0);
            row.values[120] = TableValue::make_null();
            row.values[121] = TableValue::make_int(0);
            for (int c = 122; c <= 132; c++) row.values[c] = TableValue::make_int(0);
        }

        // Assets (Col 119: Icon Name, Col 133: Model AID)
        if (const auto* assets = cls.get("assets"); assets && assets->is_obj()) {
            std::string icon_name = assets->str_or("icon_name");
            if (!icon_name.empty()) row.values[119] = TableValue::make_string(icon_name);
            std::string model_aid = assets->str_or("model_aid");
            if (!model_aid.empty()) row.values[133] = TableValue::make_string(model_aid);
        }

        // Direct column overrides if specified via "column_overrides"
        if (const auto* col_ov = cls.get("column_overrides"); col_ov && col_ov->is_obj()) {
            for (const auto& [col_str, jval] : col_ov->obj_val) {
                int col_i = std::atoi(col_str.c_str());
                if (col_i >= 0 && static_cast<size_t>(col_i) < job_sheet.fields.size()) {
                    row.values[static_cast<size_t>(col_i)] = json_to_table_value(jval, job_sheet.fields[static_cast<size_t>(col_i)]);
                }
            }
        }
    }

    // 2. Second pass: apply progression relinks (`relink_progression`) onto existing classes
    for (const auto& cls : classes->arr_val) {
        if (!cls.is_obj()) continue;
        const auto* relink = cls.get("relink_progression");
        if (!relink || !relink->is_obj()) continue;
        for (const auto& [target_jid, links] : relink->obj_val) {
            if (!links.is_obj()) continue;
            TableRow* target_row = job_sheet.find_row(target_jid);
            if (!target_row || target_row->values.size() < 10) continue;
            if (const auto* lo = links.get("lower"); lo && lo->is_str()) {
                target_row->values[7] = TableValue::make_string(lo->str_val);
            }
            if (const auto* up = links.get("upper"); up && up->is_str()) {
                target_row->values[8] = TableValue::make_string(up->str_val);
            }
        }
    }
}

void apply_skill_manifest_to_skill_table(GameTable& skill_sheet, const JsonVal& manifest) {
    const auto* skills = manifest.get("custom_skills");
    if (!skills || !skills->is_arr()) return;

    for (const auto& sk : skills->arr_val) {
        if (!sk.is_obj()) continue;
        std::string key = sk.str_or("key");
        std::string clone_from = sk.str_or("clone_from", "SID_高速飛行術");
        if (key.empty()) continue;

        TableRow& row = skill_sheet.upsert_keyed_row(key, clone_from);
        if (row.values.size() < 155) continue;

        row.values[0] = TableValue::make_string(sk.str_or("name_id"));
        row.values[1] = TableValue::make_null();
        row.values[2] = TableValue::make_string(sk.str_or("help_id"));
        std::string arg0 = sk.str_or("arg0");
        if (!arg0.empty()) {
            row.values[3] = TableValue::make_array1d({TableValue::make_string(arg0)});
        }
        row.values[8] = TableValue::make_int(1);     // Passive / Equippable skill
        row.values[12] = TableValue::make_int(1);    // Learn priority
        row.values[15] = TableValue::make_int(140);  // Equip priority
        if (const auto* tm = sk.get("timing")) row.values[19] = TableValue::make_int(tm->int_val);
        if (const auto* st = sk.get("stance")) row.values[20] = TableValue::make_int(st->int_val);
        row.values[30] = TableValue::make_array1d({});  // Clear required skill lock
        row.values[34] = TableValue::make_array2d({});  // Clear invalidating skill lock
        row.values[37] = TableValue::make_flags(0x0BFF, 16);

        // Reset inherited stat buffs from prototype before applying custom buffs
        for (int c = 56; c <= 65; c++) row.values[c] = TableValue::make_int(0);
        if (const auto* buffs = sk.get("buffs"); buffs && buffs->is_obj()) {
            if (const auto* v = buffs->get("atk")) row.values[56] = TableValue::make_int(v->int_val);
            if (const auto* v = buffs->get("phys_atk")) row.values[57] = TableValue::make_int(v->int_val);
            if (const auto* v = buffs->get("mag_atk")) row.values[58] = TableValue::make_int(v->int_val);
            if (const auto* v = buffs->get("def")) row.values[59] = TableValue::make_int(v->int_val);
            if (const auto* v = buffs->get("res")) row.values[60] = TableValue::make_int(v->int_val);
            if (const auto* v = buffs->get("hit")) row.values[61] = TableValue::make_int(v->int_val);
            if (const auto* v = buffs->get("avo")) row.values[62] = TableValue::make_int(v->int_val);
            if (const auto* v = buffs->get("crit")) row.values[63] = TableValue::make_int(v->int_val);
            if (const auto* v = buffs->get("ddg")) row.values[64] = TableValue::make_int(v->int_val);
            if (const auto* v = buffs->get("as")) row.values[65] = TableValue::make_int(v->int_val);
        }

        row.values[140] = TableValue::make_flags(0x000F, 16);
        std::string proto_icon = sk.str_or("proto_icon");
        if (!proto_icon.empty()) {
            row.values[154] = TableValue::make_string(proto_icon);
        } else {
            row.values[154] = TableValue::make_string("IconSkill_" + key);
        }

        // Direct column overrides if specified via "column_overrides"
        if (const auto* col_ov = sk.get("column_overrides"); col_ov && col_ov->is_obj()) {
            for (const auto& [col_str, jval] : col_ov->obj_val) {
                int col_i = std::atoi(col_str.c_str());
                if (col_i >= 0 && static_cast<size_t>(col_i) < skill_sheet.fields.size()) {
                    row.values[static_cast<size_t>(col_i)] = json_to_table_value(jval, skill_sheet.fields[static_cast<size_t>(col_i)]);
                }
            }
        }
    }
}

void apply_class_manifest_to_asset_table(std::vector<GameTable>& asset_sheets, const JsonVal& manifest) {
    if (asset_sheets.size() < 5) return;
    const auto* classes = manifest.get("classes");
    if (!classes || !classes->is_arr()) return;

    GameTable& s0 = asset_sheets[0];
    GameTable& s4 = asset_sheets[4];

    std::map<std::string, std::string> clause_replacements;
    for (const auto& cls : classes->arr_val) {
        if (!cls.is_obj()) continue;
        std::string key = cls.str_or("key");
        std::string clone_from = cls.str_or("clone_base");
        if (const auto* assets = cls.get("assets"); assets && assets->is_obj()) {
            std::string ac = assets->str_or("clone_from");
            if (!ac.empty()) clone_from = ac;
        }
        if (key.empty() || clone_from.empty()) continue;

        // 1. Sheet 4 (条件セット): clone row
        s4.upsert_keyed_row(key, clone_from);

        // 2. Register 2D clause replacements for Sheet 0
        clause_replacements[clone_from] = key;
        clause_replacements["!" + clone_from] = "!" + key;
    }

    if (!clause_replacements.empty()) {
        s0.clone_2d_array_clauses(0, clause_replacements);
    }
}

void apply_generic_table_patches(std::vector<GameTable>& sheets, const JsonVal& patches_arr) {
    if (!patches_arr.is_arr()) return;
    for (const auto& op : patches_arr.arr_val) {
        if (!op.is_obj()) continue;
        int sheet_idx = static_cast<int>(op.int_or("sheet", 0));
        if (sheet_idx < 0 || static_cast<size_t>(sheet_idx) >= sheets.size()) continue;
        GameTable& sheet = sheets[static_cast<size_t>(sheet_idx)];
        std::string type = op.str_or("op");

        if (type == "UPSERT_KEYED_ROW") {
            std::string key = op.str_or("key");
            std::string clone_from = op.str_or("clone_from");
            if (key.empty()) continue;
            TableRow& row = sheet.upsert_keyed_row(key, clone_from);
            if (const auto* cols = op.get("columns"); cols && cols->is_obj()) {
                for (const auto& [c_str, jval] : cols->obj_val) {
                    int ci = std::atoi(c_str.c_str());
                    if (ci >= 0 && static_cast<size_t>(ci) < sheet.fields.size()) {
                        row.values[static_cast<size_t>(ci)] = json_to_table_value(jval, sheet.fields[static_cast<size_t>(ci)]);
                    }
                }
            }
        } else if (type == "PATCH_EXISTING_ROW") {
            std::string key = op.str_or("target_key");
            TableRow* row = sheet.find_row(key);
            if (!row) continue;
            if (const auto* cols = op.get("columns"); cols && cols->is_obj()) {
                for (const auto& [c_str, jval] : cols->obj_val) {
                    int ci = std::atoi(c_str.c_str());
                    if (ci >= 0 && static_cast<size_t>(ci) < sheet.fields.size()) {
                        row->values[static_cast<size_t>(ci)] = json_to_table_value(jval, sheet.fields[static_cast<size_t>(ci)]);
                    }
                }
            }
        } else if (type == "PATCH_ROW_WHERE") {
            // Keyless rows matched by a column's value: { "op": "PATCH_ROW_WHERE", "sheet": 0, "match_column": 0,
            //   "equals": "機能開放_資格試験", "columns": { "1": 1 } } -- patches every matching row
            int mc = static_cast<int>(op.int_or("match_column", 0));
            std::string want = op.str_or("equals");
            if (mc < 0 || static_cast<size_t>(mc) >= sheet.fields.size()) continue;
            for (auto& r : sheet.rows) {
                if (static_cast<size_t>(mc) >= r.values.size() || r.values[static_cast<size_t>(mc)].to_string() != want) {
                    continue;
                }
                if (const auto* cols = op.get("columns"); cols && cols->is_obj()) {
                    for (const auto& [c_str, jval] : cols->obj_val) {
                        int ci = std::atoi(c_str.c_str());
                        if (ci >= 0 && static_cast<size_t>(ci) < sheet.fields.size()) {
                            r.values[static_cast<size_t>(ci)] = json_to_table_value(jval, sheet.fields[static_cast<size_t>(ci)]);
                        }
                    }
                }
            }
        } else if (type == "INSERT_GROUP_ROW") {
            // Grouped sheets (HubDispos): { "op": "INSERT_GROUP_ROW", "sheet": 0, "group": "キャラ_A1",
            //   "unique_column": 1, "columns": { "0": "PID_...", "1": "label", ... } }
            // Idempotent: skipped when the group already has a row with the same value in `unique_column`.
            std::string group = op.str_or("group");
            int uniq = static_cast<int>(op.int_or("unique_column", -1));
            std::map<size_t, TableValue> new_vals;
            if (const auto* cols = op.get("columns"); cols && cols->is_obj()) {
                for (const auto& [c_str, jval] : cols->obj_val) {
                    int ci = std::atoi(c_str.c_str());
                    if (ci >= 0 && static_cast<size_t>(ci) < sheet.fields.size()) {
                        new_vals[static_cast<size_t>(ci)] = json_to_table_value(jval, sheet.fields[static_cast<size_t>(ci)]);
                    }
                }
            }
            bool duplicate = false;
            if (uniq >= 0) {
                auto it = new_vals.find(static_cast<size_t>(uniq));
                for (const auto& r : sheet.rows) {
                    if (it != new_vals.end() && r.group == group && static_cast<size_t>(uniq) < r.values.size() &&
                        r.values[static_cast<size_t>(uniq)].to_string() == it->second.to_string()) {
                        duplicate = true;
                        break;
                    }
                }
            }
            if (duplicate) continue;
            // clone a row of the group (default: its first) so array columns start as valid empty arrays
            const TableRow* proto = nullptr;
            int ci_src = static_cast<int>(op.int_or("clone_group_index", 0));
            for (const auto& r : sheet.rows) {
                if (r.group == group && r.sub == ci_src) {
                    proto = &r;
                    break;
                }
            }
            if (TableRow* row = sheet.insert_group_row(group, proto)) {
                for (auto& [ci, v] : new_vals) row->values[ci] = std::move(v);
            }
        } else if (type == "SET_GROUP_ROWS") {
            // Grouped sheets (Growth 技能習得 learnsets ...): { "op": "SET_GROUP_ROWS", "sheet": 3,
            //   "group": "LID_new", "proto_group": "LID_existing", "rows": [ { "0": 1, "3": "SID_..." }, ... ] }
            // Replaces the group's rows (the group is created at the end when missing). Each row starts from the
            // first row of `proto_group` (or `group`) with scalars nulled and arrays emptied, then gets its columns.
            std::string group = op.str_or("group");
            std::string pg = op.str_or("proto_group", group);
            const TableRow* proto = nullptr;
            for (const auto& r : sheet.rows) {
                if (r.group == pg) {
                    proto = &r;
                    break;
                }
            }
            if (group.empty() || !proto) continue;
            TableRow blank = *proto;
            for (auto& val : blank.values) {
                if (val.kind == TableValue::Kind::kArray1D) {
                    val.arr_1d.clear();
                } else if (val.kind == TableValue::Kind::kArray2D) {
                    val.arr_2d.clear();
                } else {
                    val = TableValue::make_null();
                }
            }
            std::vector<TableRow> new_rows;
            if (const auto* rs = op.get("rows"); rs && rs->is_arr()) {
                for (const auto& rj : rs->arr_val) {
                    if (!rj.is_obj()) continue;
                    TableRow nr = blank;
                    for (const auto& [c_str, jval] : rj.obj_val) {
                        int ci = std::atoi(c_str.c_str());
                        if (ci >= 0 && static_cast<size_t>(ci) < sheet.fields.size() &&
                            static_cast<size_t>(ci) < nr.values.size()) {
                            nr.values[static_cast<size_t>(ci)] =
                                json_to_table_value(jval, sheet.fields[static_cast<size_t>(ci)]);
                        }
                    }
                    new_rows.push_back(std::move(nr));
                }
            }
            sheet.set_group_rows(group, std::move(new_rows));
        } else if (type == "APPEND_ROW") {
            // Keyless/condition rows (AssetTable sheet 0 ...): { "op": "APPEND_ROW", "sheet": 0, "key": "",
            //   "clone_from_clause": { "column": 0, "clause": ["AID_..."] }, "columns": { "0": [["AID_new"]], ... } }
            // Idempotent: skipped when a row with identical column-0 value already exists.
            const TableRow* proto = nullptr;
            if (const auto* cf = op.get("clone_from_clause"); cf && cf->is_obj()) {
                std::vector<std::string> clause;
                if (const auto* cl = cf->get("clause"); cl && cl->is_arr()) {
                    for (const auto& s : cl->arr_val) {
                        if (s.is_str()) clause.push_back(s.str_val);
                    }
                }
                proto = sheet.find_row_by_clause(static_cast<size_t>(cf->int_or("column", 0)), clause);
            }
            std::map<size_t, TableValue> new_vals;
            if (const auto* cols = op.get("columns"); cols && cols->is_obj()) {
                for (const auto& [c_str, jval] : cols->obj_val) {
                    int ci = std::atoi(c_str.c_str());
                    if (ci >= 0 && static_cast<size_t>(ci) < sheet.fields.size()) {
                        new_vals[static_cast<size_t>(ci)] = json_to_table_value(jval, sheet.fields[static_cast<size_t>(ci)]);
                    }
                }
            }
            bool duplicate = false;
            if (auto it = new_vals.find(0); it != new_vals.end() && it->second.kind == TableValue::Kind::kArray2D) {
                for (const auto& r : sheet.rows) {
                    if (!r.values.empty() && r.values[0].kind == TableValue::Kind::kArray2D &&
                        r.values[0].to_string() == it->second.to_string()) {
                        duplicate = true;
                        break;
                    }
                }
            }
            if (duplicate) continue;
            TableRow& row = sheet.append_row(op.str_or("key"), proto);
            for (auto& [ci, v] : new_vals) {
                row.values[ci] = std::move(v);
            }
        } else if (type == "CLONE_2D_ARRAY_CLAUSES") {
            int col_idx = static_cast<int>(op.int_or("column", 0));
            std::map<std::string, std::string> repl;
            if (const auto* r_obj = op.get("replacements"); r_obj && r_obj->is_obj()) {
                for (const auto& [k, v] : r_obj->obj_val) {
                    if (v.is_str()) repl[k] = v.str_val;
                }
            }
            if (!repl.empty() && col_idx >= 0) {
                sheet.clone_2d_array_clauses(static_cast<size_t>(col_idx), repl);
            }
        }
    }
}

void collect_localization_entries(const JsonVal& manifest,
                                  std::map<std::string, std::map<std::string, std::string>>& lang_to_job_msgs,
                                  std::map<std::string, std::map<std::string, std::string>>& lang_to_skill_msgs) {
    // 1. Class localizations -> Job.msbt.zst
    const auto* classes = manifest.get("classes");
    const auto* loc = manifest.get("localization");
    if (classes && classes->is_arr() && loc && loc->is_obj()) {
        for (const auto& cls : classes->arr_val) {
            if (!cls.is_obj()) continue;
            std::string name_id = cls.str_or("name_id");
            std::string help_id = cls.str_or("help_id");
            for (const auto& [lang, l_obj] : loc->obj_val) {
                if (!l_obj.is_obj()) continue;
                if (!name_id.empty()) lang_to_job_msgs[lang][name_id] = l_obj.str_or("name");
                if (!help_id.empty()) lang_to_job_msgs[lang][help_id] = l_obj.str_or("help");
            }
        }
    }

    // 2. Skill localizations -> Skill.msbt.zst
    const auto* skills = manifest.get("custom_skills");
    if (skills && skills->is_arr()) {
        for (const auto& sk : skills->arr_val) {
            if (!sk.is_obj()) continue;
            std::string name_id = sk.str_or("name_id");
            std::string help_id = sk.str_or("help_id");
            const auto* sk_loc = sk.get("localization");
            if (!sk_loc || !sk_loc->is_obj()) continue;
            for (const auto& [lang, l_obj] : sk_loc->obj_val) {
                if (!l_obj.is_obj()) continue;
                if (!name_id.empty()) lang_to_skill_msgs[lang][name_id] = l_obj.str_or("name");
                if (!help_id.empty()) lang_to_skill_msgs[lang][help_id] = l_obj.str_or("help");
            }
        }
    }
}

bool unpack_delta_json(const std::vector<uint8_t>& delta_bytes, std::string& out_json) {
    if (delta_bytes.size() < 64 || std::memcmp(delta_bytes.data(), "FWDL", 4) != 0) return false;
    uint32_t j_off = 0, j_sz = 0;
    std::memcpy(&j_off, delta_bytes.data() + 16, 4);
    std::memcpy(&j_sz, delta_bytes.data() + 20, 4);
    if (static_cast<size_t>(j_off) + j_sz > delta_bytes.size()) return false;
    out_json.assign(reinterpret_cast<const char*>(delta_bytes.data() + j_off), j_sz);
    return true;
}

struct DiscoveredMod {
    std::string id;
    std::string mod_root;
    JsonVal manifest;
};

std::string res_path(const std::string& mod_root, const char* file_name) {
    return path_join(path_join(mod_root, "resources"), file_name);
}
std::string res_path(const std::string& mod_root, const std::string& file_name) {
    return path_join(path_join(mod_root, "resources"), file_name);
}

void discover_mods_in_overlay(const std::string& ov_dir,
                              std::vector<DiscoveredMod>& out_mods,
                              std::set<std::string>& seen_ids) {
    // Check 1: manifest.json in ov_dir or its parent/grandparent (e.g. <pkg>/romfs -> <pkg>/manifest.json)
    std::vector<std::string> candidate_manifests;
    {
        std::string dir = ov_dir;
        for (int depth = 0; depth < 4 && !dir.empty(); ++depth) {
            candidate_manifests.push_back(path_join(dir, "manifest.json"));
            std::string parent = path_parent(dir);
            if (parent == dir) break;
            dir = parent;
        }
    }

    // Check 2: Mods/<mod_id>/manifest.json inside ov_dir
    std::string mods_sub = path_join(ov_dir, "Mods");
    std::vector<HostDirEntry> entries;
    if (g_host_fs->list_dir && g_host_fs->list_dir(mods_sub, entries)) {
        for (const auto& entry : entries) {
            if (entry.is_dir) {
                candidate_manifests.push_back(path_join(path_join(mods_sub, entry.name), "manifest.json"));
            }
        }
    }

    for (const auto& mp : candidate_manifests) {
        std::vector<uint8_t> raw;
        if (read_host_file(mp, raw)) {
            std::string s(reinterpret_cast<const char*>(raw.data()), raw.size());
            JsonVal jv;
            if (JsonParser(s).parse(jv) && jv.is_obj()) {
                std::string id = jv.str_or("id");
                if (!id.empty() && jv.bool_or("enabled", true) && seen_ids.insert(id).second) {
                    out_mods.push_back({id, path_parent(mp), std::move(jv)});
                }
            }
        }
    }

    // Check 3: GameData/Classes/*.delta inside ov_dir
    std::string classes_sub = path_join(path_join(ov_dir, "GameData"), "Classes");
    entries.clear();
    if (g_host_fs->list_dir && g_host_fs->list_dir(classes_sub, entries)) {
        std::sort(entries.begin(), entries.end(),
                  [](const HostDirEntry& a, const HostDirEntry& b) { return a.name < b.name; });
        for (const auto& entry : entries) {
            if (!entry.is_dir && path_has_ext(entry.name, ".delta")) {
                std::vector<uint8_t> delta_raw;
                std::string json_str;
                if (read_host_file(path_join(classes_sub, entry.name), delta_raw) && unpack_delta_json(delta_raw, json_str)) {
                    JsonVal jv;
                    if (JsonParser(json_str).parse(jv) && jv.is_obj()) {
                        std::string id = jv.str_or("id");
                        if (!id.empty() && jv.bool_or("enabled", true) && seen_ids.insert(id).second) {
                            out_mods.push_back({id, classes_sub, std::move(jv)});
                        }
                    }
                }
            }
        }
    }
}

}  // namespace

#ifndef FEFW_FOUNDATION_NO_STD_FILESYSTEM
HostFs make_std_host_fs() {
    auto to_path = [](const std::string& s) {
        return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
    };
    HostFs fs;
    fs.read_file = [to_path](const std::string& p, std::vector<uint8_t>& out) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(to_path(p), ec)) return false;
        std::ifstream f(to_path(p), std::ios::binary | std::ios::ate);
        if (!f) return false;
        auto sz = f.tellg();
        if (sz < 0) return false;
        out.resize(static_cast<size_t>(sz));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(out.data()), sz);
        return f.good();
    };
    fs.list_dir = [to_path](const std::string& dir, std::vector<HostDirEntry>& out) {
        std::error_code ec;
        if (!std::filesystem::is_directory(to_path(dir), ec)) return false;
        for (const auto& entry : std::filesystem::directory_iterator(to_path(dir), ec)) {
            auto u8 = entry.path().filename().u8string();
            out.push_back({std::string(reinterpret_cast<const char*>(u8.data()), u8.size()), entry.is_directory(ec)});
        }
        return true;
    };
    return fs;
}

bool apply_vfs_overlays(const std::vector<std::string>& overlay_dirs,
                        const ReadVirtualFileFn& read_base_file,
                        VfsOverlayResult& out_result,
                        std::string* err) {
    HostFs fs = make_std_host_fs();
    return apply_vfs_overlays(overlay_dirs, read_base_file, fs, out_result, err);
}
#endif

struct VfsOverlayPlan::Impl {
    std::vector<DiscoveredMod> mods;
    bool touches_job = false;
    bool touches_skill = false;
    bool touches_asset = false;
    std::map<std::string, std::vector<JsonVal>> custom_table_patches;
    std::map<std::string, std::map<std::string, std::string>> lang_to_job_msgs;
    std::map<std::string, std::map<std::string, std::string>> lang_to_skill_msgs;
    std::vector<BntxTexturePatch> job_icon_patches;
    std::vector<BntxTexturePatch> skill_icon_patches;
    // Generic message patches: file stem (e.g. "Person", "LG_Common") -> lang -> label -> text, plus ATR specs.
    std::map<std::string, std::map<std::string, std::map<std::string, std::string>>> msbt_text;
    std::map<std::string, std::map<std::string, MsbtAtrSpec>> msbt_atr;
    // Generic texture additions to SARC/BNTX archives: vpath -> patches
    std::map<std::string, std::vector<BntxTexturePatch>> bntx_patches;
};

bool VfsOverlayPlan::targets(const std::string& vpath) const { return target_paths.count(vpath) != 0; }
VfsOverlayPlan::~VfsOverlayPlan() { delete impl; }

bool plan_vfs_overlays(const std::vector<std::string>& overlay_dirs,
                       const HostFs& host_fs,
                       VfsOverlayPlan& plan,
                       std::string* err) {
    (void)err;
    plan.applied_mods.clear();
    plan.target_paths.clear();
    delete plan.impl;
    plan.impl = new VfsOverlayPlan::Impl();
    auto& I = *plan.impl;

    struct HostFsScope {
        explicit HostFsScope(const HostFs* fs) { g_host_fs = fs; }
        ~HostFsScope() { g_host_fs = nullptr; }
    } host_fs_scope(&host_fs);

    std::set<std::string> seen_ids;
    for (const auto& ov : overlay_dirs) {
        discover_mods_in_overlay(ov, I.mods, seen_ids);
    }
    const auto& mods = I.mods;
    auto& touches_job = I.touches_job;
    auto& touches_skill = I.touches_skill;
    auto& touches_asset = I.touches_asset;
    auto& custom_table_patches = I.custom_table_patches;
    auto& lang_to_job_msgs = I.lang_to_job_msgs;
    auto& lang_to_skill_msgs = I.lang_to_skill_msgs;
    auto& job_icon_patches = I.job_icon_patches;
    auto& skill_icon_patches = I.skill_icon_patches;
    auto& msbt_text = I.msbt_text;
    auto& msbt_atr = I.msbt_atr;

    for (const auto& m : mods) {
        plan.applied_mods.push_back(m.id);
        if (const auto* c = m.manifest.get("classes"); c && c->is_arr() && !c->arr_val.empty()) {
            touches_job = true;
            touches_asset = true;

        }
        // Class icons: "job_icons": [{"key": "JID_X女", "clone_from": "JID_Y女", "texture": "x.swizzled" | [candidates]}]
        // The first texture file that exists in the mod's resources is used; none -> the donor icon is cloned as is.
        if (const auto* ji = m.manifest.get("job_icons"); ji && ji->is_arr()) {
            touches_job = true;
            for (const auto& e : ji->arr_val) {
                std::string key = e.str_or("key"), donor = e.str_or("clone_from");
                if (key.empty() || donor.empty()) continue;
                std::vector<uint8_t> tex;
                if (const auto* t = e.get("texture")) {
                    if (t->is_arr()) {
                        for (const auto& cand : t->arr_val) {
                            if (!tex.empty()) break;
                            read_host_file(res_path(m.mod_root, cand.str_val), tex);
                        }
                    } else if (!t->str_val.empty()) {
                        read_host_file(res_path(m.mod_root, t->str_val), tex);
                    }
                }
                job_icon_patches.push_back({key, donor, tex});
            }
        }
        if (const auto* sk = m.manifest.get("custom_skills"); sk && sk->is_arr() && !sk->arr_val.empty()) {
            touches_skill = true;
            for (const auto& s_entry : sk->arr_val) {
                std::string sk_key = s_entry.str_or("key");
                std::string proto_icon = s_entry.str_or("proto_icon");
                if (proto_icon.empty() && !sk_key.empty()) {
                    std::vector<uint8_t> sw_bc7;
                    std::string tex_fn = s_entry.str_or("icon_texture");
                    if (!tex_fn.empty()) {
                        std::string base = tex_fn;
                        if (base.size() > 4 && base.rfind(".png") == base.size() - 4) {
                            base = base.substr(0, base.size() - 4);
                        }
                        read_host_file(res_path(m.mod_root, base + ".bc7.swizzled"), sw_bc7);
                        if (sw_bc7.empty()) {
                            read_host_file(res_path(m.mod_root, base + ".swizzled"), sw_bc7);
                        }
                    }
                    // Donor icon (layout / format template): "icon_clone_from", default a stock skill icon.
                    std::string clone_target = s_entry.str_or("icon_clone_from", "IconSkill_SID_はやぶさ");
                    skill_icon_patches.push_back({"IconSkill_" + sk_key, clone_target, sw_bc7});
                }
            }
        }

        // Generic arbitrary `.bin` table patches ("table_patches": { "/GameData/Item.bin": [ ... ] })
        if (const auto* tp = m.manifest.get("table_patches"); tp && tp->is_obj()) {
            for (const auto& [vpath, ops] : tp->obj_val) {
                if (ops.is_arr()) {
                    custom_table_patches[vpath].push_back(ops);
                }
            }
        }

        // Generic message patches:
        // "msbt_patches": { "<file stem>": { "labels": { "<label>": {
        //      "text": { "USen": "...", "JPja": "...", ... },            (missing languages fall back to USen)
        //      "atr": { "template": "<existing label>", "strings": { "12": "speaker", ... } } } } } }
        if (const auto* mp = m.manifest.get("msbt_patches"); mp && mp->is_obj()) {
            static const char* kLangs[] = {"CNzh", "EUde", "EUen", "EUes", "EUfr", "EUit", "JPja",
                                           "KRko", "TWzh", "USen", "USes", "USfr"};
            for (const auto& [stem, spec] : mp->obj_val) {
                const auto* labels = spec.get("labels");
                if (!labels || !labels->is_obj()) continue;
                for (const auto& [label, lv] : labels->obj_val) {
                    if (const auto* tx = lv.get("text"); tx && tx->is_obj()) {
                        for (const char* lang : kLangs) {
                            std::string t = tx->str_or(lang);
                            if (t.empty()) t = tx->str_or("USen");
                            if (t.empty() && !tx->obj_val.empty() && tx->obj_val.begin()->second.is_str()) {
                                t = tx->obj_val.begin()->second.str_val;
                            }
                            msbt_text[stem][lang][label] = t;
                        }
                    }
                    if (const auto* at = lv.get("atr"); at && at->is_obj()) {
                        MsbtAtrSpec s;
                        s.template_label = at->str_or("template");
                        if (const auto* ss = at->get("strings"); ss && ss->is_obj()) {
                            for (const auto& [k, v] : ss->obj_val) {
                                if (v.is_str()) s.string_overrides[std::atoi(k.c_str())] = v.str_val;
                            }
                        }
                        msbt_atr[stem][label] = std::move(s);
                    }
                }
            }
        }

        // Generic texture additions:
        // "bntx_patches": { "/Layout/Textures/Face.arc.zst": [
        //      { "name": "FID_X", "clone_from": "FID_Y", "data": "<file in resources/, swizzled, same size as Y>" } ] }
        if (const auto* bp = m.manifest.get("bntx_patches"); bp && bp->is_obj()) {
            for (const auto& [vpath, list] : bp->obj_val) {
                if (!list.is_arr()) continue;
                for (const auto& e : list.arr_val) {
                    BntxTexturePatch tp;
                    tp.new_name = e.str_or("name");
                    tp.clone_from_name = e.str_or("clone_from");
                    std::string data = e.str_or("data");
                    if (!data.empty()) read_host_file(res_path(m.mod_root, data), tp.custom_swizzled_data);
                    if (!tp.new_name.empty() && !tp.clone_from_name.empty()) {
                        I.bntx_patches[vpath].push_back(std::move(tp));
                    }
                }
            }
        }

        collect_localization_entries(m.manifest, lang_to_job_msgs, lang_to_skill_msgs);
    }


    // Every file some mod patches (built on demand by synthesize_vfs_file).
    auto& T = plan.target_paths;
    if (touches_job) T.insert("/GameData/Job.bin");
    if (touches_skill) T.insert("/GameData/Skill.bin");
    if (touches_asset) T.insert("/GameData/AssetTable.bin");
    for (const auto& kv : custom_table_patches) T.insert(kv.first);
    for (const auto& kv : lang_to_job_msgs) T.insert("/Msg/" + kv.first + "/Job.msbt.zst");
    for (const auto& kv : lang_to_skill_msgs) T.insert("/Msg/" + kv.first + "/Skill.msbt.zst");
    for (const auto& [stem, per_lang] : msbt_text) {
        for (const auto& kv : per_lang) T.insert("/Msg/" + kv.first + "/" + stem + ".msbt.zst");
    }
    if (!job_icon_patches.empty()) T.insert("/Layout/Textures/Icon_Job.arc.zst");
    if (!skill_icon_patches.empty()) T.insert("/Layout/Textures/Icon_Skill.arc.zst");
    for (const auto& kv : I.bntx_patches) T.insert(kv.first);
    return true;
}

bool synthesize_vfs_file(const VfsOverlayPlan& plan,
                         const std::string& vpath,
                         const ReadVirtualFileFn& read_base_file,
                         std::vector<uint8_t>& out,
                         std::string* err) {
    out.clear();
    if (!plan.impl || !plan.targets(vpath)) return false;
    const auto& I = *plan.impl;
    const auto& mods = I.mods;
    bool have = false;  // `out` holds this file's bytes so far (later stages build on earlier ones)

    // 1-3. class / skill manifests on Job.bin, Skill.bin, AssetTable.bin
    if (vpath == "/GameData/Job.bin" && I.touches_job) {
        std::vector<uint8_t> base;
        std::vector<GameTable> sheets;
        if (read_base_file(vpath, base) && GameTable::read_sheets(base, sheets, true, err) && !sheets.empty()) {
            for (const auto& m : mods) apply_class_manifest_to_job_table(sheets[0], m.manifest);
            out = GameTable::write_sheets(sheets);
            have = true;
        }
    } else if (vpath == "/GameData/Skill.bin" && I.touches_skill) {
        std::vector<uint8_t> base;
        std::vector<GameTable> sheets;
        if (read_base_file(vpath, base) && GameTable::read_sheets(base, sheets, true, err) && !sheets.empty()) {
            for (const auto& m : mods) apply_skill_manifest_to_skill_table(sheets[0], m.manifest);
            out = GameTable::write_sheets(sheets);
            have = true;
        }
    } else if (vpath == "/GameData/AssetTable.bin" && I.touches_asset) {
        std::vector<uint8_t> base;
        std::vector<GameTable> sheets;
        if (read_base_file(vpath, base) && GameTable::read_sheets(base, sheets, true, err) && sheets.size() >= 5) {
            for (const auto& m : mods) apply_class_manifest_to_asset_table(sheets, m.manifest);
            out = GameTable::write_sheets(sheets);
            have = true;
        }
    }

    // 4. generic table patches (Item.bin, Person.bin, ...), on top of 1-3 when they share a file
    if (auto it = I.custom_table_patches.find(vpath); it != I.custom_table_patches.end()) {
        std::vector<uint8_t> base;
        if (have) {
            base = out;
        } else if (!read_base_file(vpath, base)) {
            return false;
        }
        std::vector<GameTable> sheets;
        if (GameTable::read_sheets(base, sheets, true, err)) {
            for (const auto& ops : it->second) apply_generic_table_patches(sheets, ops);
            out = GameTable::write_sheets(sheets);
            have = true;
        }
    }

    // 5. messages: /Msg/<lang>/<stem>.msbt.zst (Job/Skill names first, generic msbt_patches on top)
    const std::string msg_prefix = "/Msg/";
    const std::string msg_suffix = ".msbt.zst";
    if (vpath.compare(0, msg_prefix.size(), msg_prefix) == 0 && vpath.size() > msg_suffix.size() &&
        vpath.compare(vpath.size() - msg_suffix.size(), msg_suffix.size(), msg_suffix) == 0) {
        size_t slash = vpath.find('/', msg_prefix.size());
        if (slash != std::string::npos) {
            std::string lang = vpath.substr(msg_prefix.size(), slash - msg_prefix.size());
            std::string stem = vpath.substr(slash + 1, vpath.size() - msg_suffix.size() - slash - 1);
            const std::map<std::string, std::string>* fixed = nullptr;
            if (stem == "Job") {
                if (auto j = I.lang_to_job_msgs.find(lang); j != I.lang_to_job_msgs.end()) fixed = &j->second;
            } else if (stem == "Skill") {
                if (auto k = I.lang_to_skill_msgs.find(lang); k != I.lang_to_skill_msgs.end()) fixed = &k->second;
            }
            if (fixed) {
                std::vector<uint8_t> base, patched;
                if (read_base_file(vpath, base) && patch_msbt_archive(base, *fixed, patched, err)) {
                    out = std::move(patched);
                    have = true;
                }
            }
            if (auto st = I.msbt_text.find(stem); st != I.msbt_text.end()) {
                if (auto lt = st->second.find(lang); lt != st->second.end()) {
                    const auto atr_it = I.msbt_atr.find(stem);
                    const std::map<std::string, MsbtAtrSpec>* atr =
                        atr_it != I.msbt_atr.end() ? &atr_it->second : nullptr;
                    std::vector<uint8_t> base, patched;
                    bool base_ok = have;
                    if (have) {
                        base = out;
                    } else {
                        base_ok = read_base_file(vpath, base);
                    }
                    if (base_ok && patch_msbt_archive(base, lt->second, patched, err, atr)) {
                        out = std::move(patched);
                        have = true;
                    }
                }
            }
        }
    }

    // 6. texture archives: class/skill icons + generic bntx_patches
    std::vector<BntxTexturePatch> textures;
    if (vpath == "/Layout/Textures/Icon_Job.arc.zst") textures = I.job_icon_patches;
    if (vpath == "/Layout/Textures/Icon_Skill.arc.zst") textures = I.skill_icon_patches;
    if (auto bt = I.bntx_patches.find(vpath); bt != I.bntx_patches.end()) {
        textures.insert(textures.end(), bt->second.begin(), bt->second.end());
    }
    if (!textures.empty()) {
        std::vector<uint8_t> base, patched;
        if (read_base_file(vpath, base) && patch_sarc_bntx_archive(base, textures, patched, err)) {
            out = std::move(patched);
            have = true;
        }
    }
    return have;
}

bool apply_vfs_overlays(const std::vector<std::string>& overlay_dirs,
                        const ReadVirtualFileFn& read_base_file,
                        const HostFs& host_fs,
                        VfsOverlayResult& out_result,
                        std::string* err) {
    out_result.synthesized_files.clear();
    out_result.applied_mods.clear();
    VfsOverlayPlan plan;
    if (!plan_vfs_overlays(overlay_dirs, host_fs, plan, err)) return false;
    out_result.applied_mods = plan.applied_mods;
    for (const auto& vpath : plan.target_paths) {
        std::vector<uint8_t> bytes;
        if (synthesize_vfs_file(plan, vpath, read_base_file, bytes, err)) {
            out_result.synthesized_files[vpath] = std::move(bytes);
        }
    }
    return true;
}

}  // namespace fefw_foundation
