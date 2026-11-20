// Unit tests for the FEFW RuntimePatcher host library. Self-contained: no game data required.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "fefw_foundation/game_table.h"
#include "fefw_foundation/msbt_patcher.h"

using namespace fefw_foundation;

static int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++g_passed;                                                          \
        } else {                                                                 \
            ++g_failed;                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

static GameTable make_table() {
    GameTable t;
    t.name = "Unit";
    t.version = 1;
    // Field offsets count from the start of the record, which begins with a 32-byte row header.
    t.fields = {
        {"Key", "string", 8, 32, 0, 0},
        {"Hp", "s32", 4, 40, 0, 0},
        {"Mov", "u8", 1, 44, 0, 0},
        {"Rate", "f32", 4, 48, 0, 0},
        {"Base", "ref", 8, 56, 0, 0},
    };
    t.row_size = 32 + 32;
    auto add = [&](const char* key, int hp, int mov, double rate, TableValue base) {
        TableRow r;
        r.key = key;
        r.index = static_cast<int>(t.rows.size());
        r.values = {TableValue::make_string(key), TableValue::make_int(hp), TableValue::make_int(mov),
                    TableValue::make_float(rate), std::move(base)};
        t.rows.push_back(std::move(r));
    };
    add("UID_A", 20, 5, 0.5, TableValue::make_null());
    add("UID_B", 30, 6, 1.25, TableValue::make_string("UID_A"));
    return t;
}

static bool roundtrip(const std::vector<GameTable>& in, std::vector<GameTable>& out, std::vector<uint8_t>& bytes) {
    bytes = GameTable::write_sheets(in);
    std::string err;
    bool ok = GameTable::read_sheets(bytes, out, true, &err);
    if (!ok) std::fprintf(stderr, "read_sheets: %s\n", err.c_str());
    return ok;
}

static void test_table_roundtrip() {
    std::vector<GameTable> out;
    std::vector<uint8_t> bytes;
    CHECK(roundtrip({make_table()}, out, bytes));
    CHECK(GameTable::is_table(bytes.data(), bytes.size()));
    if (out.size() != 1) {
        CHECK(out.size() == 1);
        return;
    }
    const GameTable& t = out[0];
    CHECK(t.name == "Unit");
    CHECK(t.rows.size() == 2);
    const TableRow* b = t.find_row("UID_B");
    CHECK(b != nullptr);
    if (b) {
        CHECK(b->values[1].int_val == 30);
        CHECK(b->values[2].int_val == 6);
        CHECK(b->values[3].float_val == 1.25);
        CHECK(b->values[4].str_val == "UID_A");
    }
    const TableRow* a = t.find_row("UID_A");
    CHECK(a && a->values[4].is_null());  // null ref stays null (not "")

    // write(read(x)) is stable
    std::vector<GameTable> out2;
    std::vector<uint8_t> bytes2;
    CHECK(roundtrip(out, out2, bytes2));
    CHECK(bytes == bytes2);
}

static void test_upsert_appends() {
    GameTable t = make_table();
    TableRow& r = t.upsert_keyed_row("UID_NEW", "UID_B");
    CHECK(r.key == "UID_NEW");
    CHECK(t.rows.size() == 3);
    CHECK(t.find_row_index("UID_NEW") == 2);  // appended, existing indices unchanged
    CHECK(t.find_row_index("UID_A") == 0);
    CHECK(t.find_row_index("UID_B") == 1);
    CHECK(t.find_row("UID_NEW")->values[1].int_val == 30);  // cloned values

    t.upsert_keyed_row("UID_NEW", "UID_A");  // existing key: no new row
    CHECK(t.rows.size() == 3);

    std::vector<GameTable> out;
    std::vector<uint8_t> bytes;
    CHECK(roundtrip({t}, out, bytes));
    CHECK(out.size() == 1 && out[0].rows.size() == 3);
}

static void test_is_table_rejects_garbage() {
    std::vector<uint8_t> junk(256, 0xAB);
    CHECK(!GameTable::is_table(junk.data(), junk.size()));
    CHECK(!GameTable::is_table(nullptr, 0));
}

static void test_zstd_roundtrip() {
    std::string text;
    for (int i = 0; i < 1000; ++i) text += "FEFW runtime patcher " + std::to_string(i) + "\n";
    std::vector<uint8_t> packed, unpacked;
    CHECK(zstd_compress(reinterpret_cast<const uint8_t*>(text.data()), text.size(), packed));
    CHECK(packed.size() < text.size());
    CHECK(zstd_decompress(packed.data(), packed.size(), unpacked));
    CHECK(std::string(unpacked.begin(), unpacked.end()) == text);
}

static void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}
static uint32_t get_u32(const std::vector<uint8_t>& v, size_t p) {
    return v[p] | (v[p + 1] << 8) | (v[p + 2] << 16) | (static_cast<uint32_t>(v[p + 3]) << 24);
}
static void add_section(std::vector<uint8_t>& f, const char* magic, std::vector<uint8_t> body) {
    f.insert(f.end(), magic, magic + 4);
    put_u32(f, static_cast<uint32_t>(body.size()));
    put_u32(f, 0);
    put_u32(f, 0);
    body.resize((body.size() + 15) & ~size_t{15}, 0xAB);
    f.insert(f.end(), body.begin(), body.end());
}
static size_t find_sec(const std::vector<uint8_t>& f, const char* magic) {
    for (size_t p = 0x20; p + 16 <= f.size();) {
        if (std::memcmp(f.data() + p, magic, 4) == 0) return p;
        p += 16 + ((get_u32(f, p + 4) + 15) & ~size_t{15});
    }
    return 0;
}

// Adding labels must keep ATR1 and TSY1 at one entry per TXT2 string.
static void test_msbt_new_label_keeps_tsy1_parallel() {
    std::vector<uint8_t> f(0x20, 0);
    std::memcpy(f.data(), "MsgStdBn", 8);
    f[0x08] = 0xFF; f[0x09] = 0xFE; f[0x0C] = 1; f[0x0D] = 3; f[0x0E] = 4;
    std::vector<uint8_t> lbl;
    put_u32(lbl, 1);  // one hash group holding both labels
    put_u32(lbl, 2);
    put_u32(lbl, 12);
    for (const char* n : {"A", "B"}) {
        lbl.push_back(1);
        lbl.push_back(static_cast<uint8_t>(n[0]));
        put_u32(lbl, n[0] == 'A' ? 0 : 1);
    }
    add_section(f, "LBL1", lbl);
    std::vector<uint8_t> atr;
    put_u32(atr, 2);
    put_u32(atr, 4);
    put_u32(atr, 0);
    put_u32(atr, 0);
    add_section(f, "ATR1", atr);
    std::vector<uint8_t> tsy;
    put_u32(tsy, 7);
    put_u32(tsy, 9);
    add_section(f, "TSY1", tsy);
    std::vector<uint8_t> txt;
    put_u32(txt, 2);
    put_u32(txt, 12);
    put_u32(txt, 16);
    for (char c : {'a', 'b'}) { txt.push_back(c); txt.push_back(0); txt.push_back(0); txt.push_back(0); }
    add_section(f, "TXT2", txt);
    add_section(f, "AOCM", {2, 0, 0xFF, 0x05});
    uint32_t total = static_cast<uint32_t>(f.size());
    std::memcpy(f.data() + 0x12, &total, 4);

    std::map<std::string, MsbtAtrSpec> specs;
    specs["D"].template_label = "B";
    std::vector<uint8_t> out;
    std::string err;
    CHECK(patch_msbt_archive(f, {{"C", "c"}, {"D", "d"}}, out, &err, &specs));
    size_t txt2 = find_sec(out, "TXT2"), tsy1 = find_sec(out, "TSY1"), atr1 = find_sec(out, "ATR1");
    CHECK(txt2 && tsy1 && atr1);
    if (!txt2 || !tsy1 || !atr1) return;
    CHECK(get_u32(out, txt2 + 16) == 4);
    CHECK(get_u32(out, atr1 + 16) == 4);
    CHECK(get_u32(out, tsy1 + 4) == 16);
    CHECK(get_u32(out, tsy1 + 16) == 7 && get_u32(out, tsy1 + 20) == 9);
    CHECK(get_u32(out, tsy1 + 24) == 7);  // no template: most common style (tie: first)
    CHECK(get_u32(out, tsy1 + 28) == 9);  // template "B": its style
    size_t aocm = find_sec(out, "AOCM");
    CHECK(aocm && get_u32(out, aocm + 4) == 6);
    if (aocm) CHECK(out[aocm + 16] == 4 && out[aocm + 20] == 0xFF && out[aocm + 21] == 0x05);
    CHECK(get_u32(out, 0x12) == out.size());
}

int main() {
    test_table_roundtrip();
    test_upsert_appends();
    test_is_table_rejects_garbage();
    test_zstd_roundtrip();
    test_msbt_new_label_keeps_tsy1_parallel();
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
