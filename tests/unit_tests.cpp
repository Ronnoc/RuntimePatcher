// Unit tests for the FEFW RuntimePatcher host library. Self-contained: no game data required.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "fefw_foundation/game_table.h"
#include "fefw_foundation/msbt_patcher.h"
#include "fefw_foundation/plugin_host_core.h"

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

static void test_settings_store() {
    SettingsStore s;
    const char* ini = "# comment\r\nalpha=1\r\nbeta=hello=world\n=bad\nbadline\nalpha=2\n";
    s.parse_ini(ini, std::strlen(ini));
    CHECK(s.count() == 2);
    char buf[64]{};
    CHECK(s.get("alpha", buf, sizeof(buf)) == 1 && std::strcmp(buf, "2") == 0);
    CHECK(s.get("beta", buf, sizeof(buf)) == 11 && std::strcmp(buf, "hello=world") == 0);
    CHECK(s.get("missing", buf, sizeof(buf)) == -1);

    CHECK(s.set("bad=key", "1") == -1);
    CHECK(s.set("bad\nkey", "1") == -1);
    CHECK(s.set("ok", "bad\nval") == -1);
    CHECK(s.set("gamma", "42") == 0);

    SettingsStore overrides;
    CHECK(overrides.set("alpha", "99") == 0);
    CHECK(overrides.set("delta", "yes") == 0);
    s.merge_from(overrides);
    CHECK(s.get("alpha", buf, sizeof(buf)) == 2 && std::strcmp(buf, "99") == 0);
    CHECK(s.get("delta", buf, sizeof(buf)) == 3 && std::strcmp(buf, "yes") == 0);

    char out[512]{};
    size_t n = s.format_ini(out, sizeof(out));
    CHECK(n > 0);
    SettingsStore round;
    round.parse_ini(out, n);
    CHECK(round.count() == s.count());
    CHECK(round.get("gamma", buf, sizeof(buf)) == 2 && std::strcmp(buf, "42") == 0);
}

struct FakeGameEnv {
    uint8_t mem[0x1000]{};
    void* fake_heap = nullptr;
    void* fake_heap_ptr = nullptr;
    std::vector<PluginMenuItem*> allocated;
    std::vector<std::pair<void*, PluginMenuItem*>> added;
    int refreshed = 0;
    int dtor_calls[2] = {0, 0};
    uint8_t fake_parent[0x100]{};
    uint8_t fake_submenu_a[0x40]{};
    uint8_t fake_submenu_b[0x40]{};
    int submenu_seq = 0;
};

static FakeGameEnv* g_fake_env = nullptr;

static uint64_t fake_call_game(uintptr_t addr, const uint64_t* args, int nargs) {
    auto* e = g_fake_env;
    uintptr_t base = reinterpret_cast<uintptr_t>(e->mem);
    if (addr == base + 0x100) {  // ui_alloc
        CHECK(nargs == 4 && args[1] == sizeof(PluginMenuItem));
        auto* item = new PluginMenuItem{};
        e->allocated.push_back(item);
        return reinterpret_cast<uint64_t>(item);
    }
    if (addr == base + 0x110) {  // ui_add_item
        auto* item = reinterpret_cast<PluginMenuItem*>(args[1]);
        *reinterpret_cast<void**>(item->native + 8) = e->fake_parent;
        e->added.push_back({reinterpret_cast<void*>(args[0]), item});
        return 0;
    }
    if (addr == base + 0x120) {  // ui_submenu
        return reinterpret_cast<uint64_t>((e->submenu_seq++ == 0) ? e->fake_submenu_a : e->fake_submenu_b);
    }
    if (addr == base + 0x130) {  // ui_refresh
        ++e->refreshed;
        return 0;
    }
    if (addr == 0xDEAD0001ull) {
        ++e->dtor_calls[0];
        return 0;
    }
    if (addr == 0xDEAD0002ull) {
        ++e->dtor_calls[1];
        return 0;
    }
    return 0;
}

static void test_menu_host_groups_and_lifecycle() {
    FakeGameEnv env;
    g_fake_env = &env;
    env.fake_heap = reinterpret_cast<void*>(0x1234);
    env.fake_heap_ptr = &env.fake_heap;
    uintptr_t base = reinterpret_cast<uintptr_t>(env.mem);
    // Store &fake_heap_ptr at base + 0x200 (ui_heap_got)
    void** heap_got = reinterpret_cast<void**>(env.mem + 0x200);
    *heap_got = env.fake_heap_ptr;
    // Fake vtable at base + 0x300 (0x248 bytes): slots 0 & 1 are orig dtors
    uint64_t* fake_vt_prefix = reinterpret_cast<uint64_t*>(env.mem + 0x300);
    fake_vt_prefix[2] = 0xDEAD0001ull;
    fake_vt_prefix[3] = 0xDEAD0002ull;

    FefwMenuConfig cfg{};
    cfg.ui_heap_got = 0x200;
    cfg.ui_alloc = 0x100;
    cfg.ui_add_item = 0x110;
    cfg.ui_submenu = 0x120;
    cfg.ui_refresh = 0x130;
    cfg.ui_item_vtable = 0x300;
    cfg.ui_item_vtable_bytes = 0x248;
    cfg.vt_text = 0x10;
    cfg.vt_confirm = 0xC8;
    cfg.vt_right = 0x138;
    cfg.vt_left = 0x140;

    MenuPlatform plat{};
    plat.main_base = base;
    plat.call_game = &fake_call_game;
    plat.slot_text = 0xA001;
    plat.slot_confirm = 0xA002;
    plat.slot_left = 0xA003;
    plat.slot_right = 0xA004;
    plat.slot_dtor[0] = 0xA005;
    plat.slot_dtor[1] = 0xA006;

    MenuHost host;
    CHECK(host.init(cfg, plat));

    std::vector<std::string> log;
    auto push_provider = [](void* user, void*) {
        auto* p = static_cast<std::pair<std::vector<std::string>*, const char*>*>(user);
        p->first->push_back(p->second);
    };
    std::pair<std::vector<std::string>*, const char*> p_root{&log, "root"};
    std::pair<std::vector<std::string>*, const char*> p_late{&log, "late"};
    std::pair<std::vector<std::string>*, const char*> p_early{&log, "early"};
    std::pair<std::vector<std::string>*, const char*> p_sg_b{&log, "sg_b"};
    std::pair<std::vector<std::string>*, const char*> p_sg_a{&log, "sg_a"};
    std::pair<std::vector<std::string>*, const char*> p_sg_dup{&log, "sg_dup"};

    auto title_fn = [](void*, void*) -> const char* { return "OptionsTitle"; };

    CHECK(host.register_system_menu_provider(push_provider, &p_root) == 1);
    CHECK(host.register_menu_group_provider_ordered("@Ronnoc", 20, push_provider, &p_late) == 1);
    CHECK(host.register_menu_group_provider_ordered("@Ronnoc", -10, push_provider, &p_early) == 1);
    CHECK(host.register_menu_subgroup_provider("@Ronnoc", "opts", 10, nullptr, nullptr, 5, "k_b", push_provider,
                                               &p_sg_b) == 1);
    CHECK(host.register_menu_subgroup_provider("@Ronnoc", "opts", 0, title_fn, nullptr, 1, "k_a", push_provider,
                                               &p_sg_a) == 1);
    // Duplicate item_key "k_a" must be ignored and return 1
    CHECK(host.register_menu_subgroup_provider("@Ronnoc", "opts", 5, nullptr, nullptr, 0, "k_a", push_provider,
                                               &p_sg_dup) == 1);

    int closed_count = 0;
    CHECK(host.register_menu_closed_callback([](void* u) { ++*static_cast<int*>(u); }, &closed_count) == 1);

    void* root_menu = reinterpret_cast<void*>(0x5555);
    host.on_system_menu(root_menu);
    CHECK(log.size() == 1 && log[0] == "root");
    CHECK(env.added.size() == 1 && env.added[0].first == root_menu);
    PluginMenuItem* group_item = env.added[0].second;
    CHECK(std::strcmp(MenuHost::handle_text(group_item), "@Ronnoc") == 0);
    CHECK(host.live_rows() == 1);

    // Step handler on enabled vs disabled row
    int step_sum = 0;
    MenuHost::set_step(group_item, [](void*, void*, int dir) -> int { return dir; });
    CHECK(MenuHost::handle_step(group_item, 1) == 1);
    group_item->native[0x52] = 0;
    CHECK(MenuHost::handle_step(group_item, 1) == 0);
    group_item->native[0x52] = 1;
    (void)step_sum;

    // Opening @Ronnoc should invoke: early (-10), subgroup "opts" (min sub_order=0), late (20)
    log.clear();
    CHECK(MenuHost::handle_confirm(group_item) == FEFW_MENU_OPENED);
    CHECK(log.size() == 2 && log[0] == "early" && log[1] == "late");
    CHECK(env.added.size() == 2 && env.added[1].first == env.fake_submenu_a);
    PluginMenuItem* sg_item = env.added[1].second;
    CHECK(std::strcmp(MenuHost::handle_text(sg_item), "OptionsTitle") == 0);
    CHECK(host.live_rows() == 2);

    // Opening subgroup should invoke sg_a (order 1), sg_b (order 5), and skip sg_dup
    log.clear();
    CHECK(MenuHost::handle_confirm(sg_item) == FEFW_MENU_OPENED);
    CHECK(log.size() == 2 && log[0] == "sg_a" && log[1] == "sg_b");

    // Destroying all rows triggers menu_closed exactly once on next flush
    host.handle_dtor(sg_item, 1);
    CHECK(host.live_rows() == 1);
    host.flush_menu_closed();
    CHECK(closed_count == 0);
    host.handle_dtor(group_item, 0);
    CHECK(host.live_rows() == 0);
    CHECK(env.dtor_calls[0] == 1 && env.dtor_calls[1] == 1);
    host.flush_menu_closed();
    CHECK(closed_count == 1);
    host.flush_menu_closed();
    CHECK(closed_count == 1);

    for (auto* p : env.allocated) delete p;
    g_fake_env = nullptr;
}

int main() {
    test_table_roundtrip();
    test_upsert_appends();
    test_is_table_rejects_garbage();
    test_zstd_roundtrip();
    test_msbt_new_label_keeps_tsy1_parallel();
    test_settings_store();
    test_menu_host_groups_and_lifecycle();
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
