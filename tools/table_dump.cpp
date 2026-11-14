// SPDX-License-Identifier: MIT
// fefw_table_dump — dump FEFW GameData tables (all sheets, grouped or inline) as text.
//
//   fefw_table_dump <table.bin> [--grep TEXT] [--sheet N] [--fields]
//   fefw_table_dump --overlay <romfs_root> <mod_romfs_dir> </GameData/X.bin> [--grep TEXT] [--sheet N]
//       runs apply_vfs_overlays with one mod package dir (the dir holding manifest.json) and dumps the
//       synthesized table, i.e. exactly what the runtime would serve to the game.
//       --write DIR also saves every synthesized file there (path separators flattened to _).
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "fefw_foundation/game_table.h"
#include "fefw_foundation/vfs_overlay.h"

using namespace fefw_foundation;

static bool read_file(const std::string& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

static void dump(const std::vector<uint8_t>& data, const std::string& grep, int only_sheet, bool fields_only) {
    std::vector<GameTable> sheets;
    std::string err;
    if (!GameTable::read_sheets(data, sheets, true, &err)) {
        std::printf("read_sheets failed: %s\n", err.c_str());
        return;
    }
    for (size_t s = 0; s < sheets.size(); ++s) {
        if (only_sheet >= 0 && static_cast<int>(s) != only_sheet) continue;
        const auto& t = sheets[s];
        std::printf("=== sheet %zu '%s' rows=%zu grouped=%d groups=%zu\n", s, t.name.c_str(), t.rows.size(),
                    t.grouped ? 1 : 0, t.groups.size());
        for (size_t i = 0; i < t.fields.size(); ++i) {
            std::printf("  field %zu %s : %s\n", i, t.fields[i].name.c_str(), t.fields[i].type_name.c_str());
        }
        if (fields_only) continue;
        for (size_t r = 0; r < t.rows.size(); ++r) {
            const auto& row = t.rows[r];
            std::string line = "[" + std::to_string(r) + "] key='" + row.key + "' group='" + row.group + "'";
            for (size_t c = 0; c < row.values.size(); ++c) {
                if (row.values[c].is_null()) continue;
                std::string v = row.values[c].to_string();
                if (v.empty() || v == "0") continue;
                line += " | " + std::to_string(c) + ":" + v;
            }
            if (!grep.empty() && line.find(grep) == std::string::npos) continue;
            std::printf("%s\n", line.c_str());
        }
    }
}

int main(int argc, char** argv) {
    std::string grep;
    int sheet = -1;
    bool fields_only = false;
    std::vector<std::string> pos;
    bool overlay = false;
    std::string write_dir;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--grep" && i + 1 < argc) grep = argv[++i];
        else if (a == "--sheet" && i + 1 < argc) sheet = std::atoi(argv[++i]);
        else if (a == "--fields") fields_only = true;
        else if (a == "--overlay") overlay = true;
        else if (a == "--write" && i + 1 < argc) write_dir = argv[++i];
        else pos.push_back(a);
    }
    if (!overlay && pos.size() == 1) {
        std::vector<uint8_t> data;
        if (!read_file(pos[0], data)) {
            std::printf("cannot read %s\n", pos[0].c_str());
            return 1;
        }
        dump(data, grep, sheet, fields_only);
        return 0;
    }
    if (overlay && pos.size() == 3) {
        const std::string romfs = pos[0];
        ReadVirtualFileFn read_base = [&](const std::string& vpath, std::vector<uint8_t>& out) {
            return read_file(romfs + vpath, out);
        };
        VfsOverlayResult res;
        std::string err;
        if (!apply_vfs_overlays({pos[1]}, read_base, res, &err)) {
            std::printf("apply_vfs_overlays failed: %s\n", err.c_str());
            return 1;
        }
        for (const auto& m : res.applied_mods) std::printf("applied mod: %s\n", m.c_str());
        for (const auto& [p, b] : res.synthesized_files) {
            std::printf("synthesized: %s (%zu bytes)\n", p.c_str(), b.size());
            if (!write_dir.empty()) {
                std::string flat = p;
                for (auto& ch : flat) {
                    if (ch == '/') ch = '_';
                }
                std::ofstream o(write_dir + "/" + flat, std::ios::binary);
                o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
            }
        }
        auto it = res.synthesized_files.find(pos[2]);
        if (it == res.synthesized_files.end()) {
            std::printf("%s not synthesized\n", pos[2].c_str());
            return 1;
        }
        dump(it->second, grep, sheet, fields_only);
        return 0;
    }
    std::printf("usage: fefw_table_dump <table.bin> [--grep T] [--sheet N] [--fields]\n"
                "       fefw_table_dump --overlay <romfs_root> <mod_dir> </GameData/X.bin> [--grep T]\n");
    return 2;
}
