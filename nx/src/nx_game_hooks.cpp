// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — game-side services for plugins (NRO loader only).
//
//   * nn::hid::GetNpadState hooks  -> per-frame callbacks + button injection
//   * System menu hook             -> plugin menu providers
//   * Menu service                 -> native menu item clones
//   * Plugin host                  -> <mount>:/fefw/plugins/*.nro loaded through nn::ro
//
// Game addresses come from the game profile (build/nx/gen/fefw_game_offsets.h, see tools/game_profile.py).
// The menu service checks a code signature before it touches anything.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fefw_foundation/plugin_api.h"
#include "fefw_game_offsets.h"
#include "nx_runtime.h"
#include "nx_settings.h"

namespace nn {
namespace hid {
struct NpadHandheldState;
struct NpadJoyDualState;
struct NpadFullKeyState;
#if FEFW_NX_NRO
void GetNpadState(NpadHandheldState*, const unsigned int&);
void GetNpadState(NpadJoyDualState*, const unsigned int&);
void GetNpadState(NpadFullKeyState*, const unsigned int&);
#endif
}  // namespace hid
#if FEFW_NX_NRO
// NRO mode: real imports, bound by nn::ro when the runtime is loaded (scanning modules for exports does not
// work under Eden's Switch 2 mode).
namespace ro {
struct RegistrationInfo;
struct Module;
uint32_t RegisterModuleInfo(RegistrationInfo*, const void*);
uint32_t GetBufferSize(unsigned long*, const void*);
uint32_t LoadModule(Module*, const void*, void*, unsigned long, int);
uint32_t LookupModuleSymbol(unsigned long*, const Module*, const char*);
}  // namespace ro
#endif
}  // namespace nn

namespace {

// ------------------------------------------------------------------------------------------------
// Game build constants (main-relative)
// ------------------------------------------------------------------------------------------------
constexpr uintptr_t kSigMenuResume = FEFW_GAME_MENU_RESUME;          // instruction after the menu hook site
constexpr uint32_t kSigMenuResumeInsn = FEFW_GAME_MENU_RESUME_INSN;
constexpr uintptr_t kUiHeapGot = FEFW_GAME_UI_HEAP_GOT;              // GOT slot -> &heap*
constexpr uintptr_t kUiAlloc = FEFW_GAME_UI_ALLOC;                   // void* alloc(heap, size, align, flags)
constexpr uintptr_t kUiSubmenu = FEFW_GAME_UI_SUBMENU;               // void* submenu(parent, const u64 info[3])
constexpr uintptr_t kUiAddItem = FEFW_GAME_UI_ADD_ITEM;              // void add_item(menu, item)
constexpr uintptr_t kUiRefresh = FEFW_GAME_UI_REFRESH;               // void refresh(item)
constexpr uintptr_t kUiItemVtablePrefix = FEFW_GAME_UI_ITEM_VTABLE;  // menu item vtable incl. offset-to-top + RTTI
constexpr size_t kUiItemVtableBytes = FEFW_GAME_UI_ITEM_VTABLE_BYTES;
constexpr size_t kVtText = FEFW_GAME_VT_TEXT, kVtConfirm = FEFW_GAME_VT_CONFIRM, kVtRight = FEFW_GAME_VT_RIGHT,
                 kVtLeft = FEFW_GAME_VT_LEFT;

uintptr_t g_main = 0;
bool g_menu_ok = false;

uintptr_t self_base() { return fefw_nx::find_module_base(reinterpret_cast<uintptr_t>(&self_base)); }

uintptr_t main_base() {
    if (g_main) return g_main;
#if FEFW_NX_NRO
    // malloc is imported from main (see the build's import report): its address lies in main's .text.
    uintptr_t m = reinterpret_cast<uintptr_t>(&malloc);
#else
    uintptr_t m = fefw_nx::lookup_symbol_in_other_modules(self_base(), "malloc");
#endif
    uintptr_t base = m ? fefw_nx::find_module_base(m) : 0;
    if (base && fefw_nx::is_readable(base + kSigMenuResume, 4)) {
        g_menu_ok = *reinterpret_cast<const uint32_t*>(base + kSigMenuResume) == kSigMenuResumeInsn;
    }
    g_main = base;
    fefw_nx::log("game services: main @ %p, menu service %s", reinterpret_cast<void*>(base),
                 g_menu_ok ? "enabled" : "DISABLED (unknown game build)");
    return base;
}

template <typename T>
T game_fn(uintptr_t off) {
    return reinterpret_cast<T>(main_base() + off);
}

volatile int g_reg_lock = 0;
void lock(volatile int* l) {
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE) != 0) fefw_nx::sleep_ns(100000);
}
void unlock(volatile int* l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

// ------------------------------------------------------------------------------------------------
// Registries
// ------------------------------------------------------------------------------------------------
constexpr int kMaxCallbacks = 16;
struct MenuProvider {
    FefwMenuProviderFn fn;
    void* user;
};
struct FrameCallback {
    FefwFrameFn fn;
    void* user;
};
MenuProvider g_menu_providers[kMaxCallbacks];
int g_menu_provider_count = 0;

constexpr int kMaxMenuGroups = 8;
struct MenuGroup {
    char name[64];
    MenuProvider providers[kMaxCallbacks];
    int count;
    FefwMenuEntry entry;
};
MenuGroup g_menu_groups[kMaxMenuGroups];
int g_menu_group_count = 0;

void* menu_add_entry(void* menu, const FefwMenuEntry* entry);
void* menu_open_submenu(void* item);

const char* group_menu_text(void* user, void*) {
    return static_cast<MenuGroup*>(user)->name;
}

int group_menu_confirm(void* user, void* item) {
    auto* g = static_cast<MenuGroup*>(user);
    void* sub = menu_open_submenu(item);
    if (!sub) return FEFW_MENU_IGNORED;
    int n = __atomic_load_n(&g->count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; ++i) g->providers[i].fn(g->providers[i].user, sub);
    return FEFW_MENU_OPENED;
}

FrameCallback g_frame_callbacks[kMaxCallbacks];
int g_frame_callback_count = 0;

struct MenuClosedCallback {
    FefwMenuClosedFn fn;
    void* user;
};
MenuClosedCallback g_menu_closed[kMaxCallbacks];
int g_menu_closed_count = 0;
int g_live_rows = 0;            // plugin menu rows the game hasn't destroyed yet (game thread only)
int g_menu_closed_pending = 0;  // set when g_live_rows reaches 0; callbacks run from the next frame tick

// call_game with x8 = result (AAPCS64 indirect result location).
uint64_t api_call_game_sret(uintptr_t addr, void* result, const uint64_t* args, int nargs) {
    uint64_t a[8] = {};
    for (int i = 0; i < nargs && i < 8; ++i) a[i] = args[i];
    register uint64_t x0 asm("x0") = a[0];
    register uint64_t x1 asm("x1") = a[1];
    register uint64_t x2 asm("x2") = a[2];
    register uint64_t x3 asm("x3") = a[3];
    register uint64_t x4 asm("x4") = a[4];
    register uint64_t x5 asm("x5") = a[5];
    register uint64_t x6 asm("x6") = a[6];
    register uint64_t x7 asm("x7") = a[7];
    register uint64_t x8 asm("x8") = reinterpret_cast<uint64_t>(result);
    register uint64_t fn asm("x16") = addr;
    asm volatile("blr x16"
                 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7), "+r"(x8), "+r"(fn)
                 :
                 : "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17", "x30", "v0", "v1", "v2", "v3", "v4", "v5",
                   "v6", "v7", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27",
                   "v28", "v29", "v30", "v31", "cc", "memory");
    return x0;
}

int register_menu_closed_callback(FefwMenuClosedFn fn, void* user) {
    lock(&g_reg_lock);
    int ok = fn && g_menu_closed_count < kMaxCallbacks;
    if (ok) g_menu_closed[g_menu_closed_count++] = {fn, user};
    unlock(&g_reg_lock);
    return ok;
}

int register_system_menu_provider(FefwMenuProviderFn fn, void* user) {
    lock(&g_reg_lock);
    int ok = fn && g_menu_provider_count < kMaxCallbacks;
    if (ok) {
        g_menu_providers[g_menu_provider_count] = {fn, user};
        __atomic_store_n(&g_menu_provider_count, g_menu_provider_count + 1, __ATOMIC_RELEASE);
    }
    unlock(&g_reg_lock);
    return ok;
}

int register_menu_group_provider(const char* group_name, FefwMenuProviderFn fn, void* user) {
    if (!group_name || !group_name[0] || strlen(group_name) >= sizeof(g_menu_groups[0].name) || !fn) return 0;
    lock(&g_reg_lock);
    int gi = -1;
    for (int i = 0; i < g_menu_group_count; ++i) {
        if (strcmp(g_menu_groups[i].name, group_name) == 0) {
            gi = i;
            break;
        }
    }
    int ok = 0;
    if (gi < 0) {
        if (g_menu_group_count < kMaxMenuGroups) {
            gi = g_menu_group_count;
            MenuGroup& g = g_menu_groups[gi];
            snprintf(g.name, sizeof(g.name), "%s", group_name);
            g.providers[0] = {fn, user};
            g.entry = {&group_menu_text, &group_menu_confirm, &g};
            __atomic_store_n(&g.count, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&g_menu_group_count, g_menu_group_count + 1, __ATOMIC_RELEASE);
            ok = 1;
        }
    } else {
        MenuGroup& g = g_menu_groups[gi];
        if (g.count < kMaxCallbacks) {
            g.providers[g.count] = {fn, user};
            __atomic_store_n(&g.count, g.count + 1, __ATOMIC_RELEASE);
            ok = 1;
        }
    }
    unlock(&g_reg_lock);
    return ok;
}

// Mid-function hook slots: slot k is the k-th `mid_hooks` entry of the game profile, entered through the export
// fefw_mid_<k>. Names (FEFW_MID_HOOK_NAMES) come from the profile, so no hook site is known to this code.
constexpr int kMidSlots = FEFW_MAX_MID_HOOKS;
constexpr int kCallbacksPerSlot = 4;
const char* const kMidHookNames[] = {FEFW_MID_HOOK_NAMES};
constexpr int kMidHookCount = static_cast<int>(sizeof(kMidHookNames) / sizeof(kMidHookNames[0])) - 1;  // last: ""
static_assert(kMidHookCount <= kMidSlots, "game profile has more mid hooks than slots");
struct MidHook {
    FefwMidHookFn fn;
    void* user;
};
MidHook g_mid_hooks[kMidSlots][kCallbacksPerSlot];
int g_mid_hook_count[kMidSlots];

int register_mid_hook(const char* name, FefwMidHookFn fn, void* user) {
    if (!name || !fn) return 0;
    int added = 0;
    lock(&g_reg_lock);
    for (int k = 0; k < kMidHookCount; ++k) {
        int n = g_mid_hook_count[k];
        if (strcmp(kMidHookNames[k], name) != 0 || n >= kCallbacksPerSlot) continue;
        g_mid_hooks[k][n] = {fn, user};
        __atomic_store_n(&g_mid_hook_count[k], n + 1, __ATOMIC_RELEASE);
        ++added;
    }
    unlock(&g_reg_lock);
    return added > 0;
}

void run_mid_hooks(int slot, uint64_t* ctx) {
    int n = __atomic_load_n(&g_mid_hook_count[slot], __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; ++i) g_mid_hooks[slot][i].fn(g_mid_hooks[slot][i].user, ctx);
}

// The System menu service: the profile's "system_menu" hook site, where the menu container is in register
// FEFW_GAME_SYSTEM_MENU_REG.
void on_system_menu(void*, uint64_t* ctx) {
    if (!main_base() || !g_menu_ok) return;
    void* menu = reinterpret_cast<void*>(ctx[FEFW_GAME_SYSTEM_MENU_REG]);
    int n = __atomic_load_n(&g_menu_provider_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; ++i) g_menu_providers[i].fn(g_menu_providers[i].user, menu);
    int gn = __atomic_load_n(&g_menu_group_count, __ATOMIC_ACQUIRE);
    for (int i = 0; i < gn; ++i) menu_add_entry(menu, &g_menu_groups[i].entry);
}

int register_frame_callback(FefwFrameFn fn, void* user) {
    lock(&g_reg_lock);
    int ok = g_frame_callback_count < kMaxCallbacks;
    if (ok) {
        g_frame_callbacks[g_frame_callback_count].fn = fn;
        g_frame_callbacks[g_frame_callback_count].user = user;
        __atomic_store_n(&g_frame_callback_count, g_frame_callback_count + 1, __ATOMIC_RELEASE);
    }
    unlock(&g_reg_lock);
    return ok;
}

// ------------------------------------------------------------------------------------------------
// Frame tick + button injection (from the game's npad polling)
// ------------------------------------------------------------------------------------------------
volatile uint64_t g_inject_buttons = 0;
volatile uint32_t g_inject_frames = 0;
volatile int g_tick_busy = 0;
uint64_t g_last_tick = 0;
uint64_t g_frame = 0;

void inject_buttons(uint64_t buttons, uint32_t frames) {
    __atomic_store_n(&g_inject_buttons, buttons, __ATOMIC_RELEASE);
    __atomic_store_n(&g_inject_frames, frames, __ATOMIC_RELEASE);
}

uint64_t counter_ticks() {
    uint64_t v;
    asm volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

// The game polls several npad ids / styles per frame: coalesce into one tick per >= 8 ms.
void frame_tick() {
    if (__atomic_exchange_n(&g_tick_busy, 1, __ATOMIC_ACQUIRE) != 0) return;
    uint64_t now = counter_ticks();
    if (now - g_last_tick >= 19200000ull / 125) {  // 19.2 MHz system counter
        g_last_tick = now;
        ++g_frame;
        uint32_t f = __atomic_load_n(&g_inject_frames, __ATOMIC_ACQUIRE);
        if (f) __atomic_store_n(&g_inject_frames, f - 1, __ATOMIC_RELEASE);
        int n = __atomic_load_n(&g_frame_callback_count, __ATOMIC_ACQUIRE);
        for (int i = 0; i < n; ++i) g_frame_callbacks[i].fn(g_frame_callbacks[i].user, g_frame);
        if (__atomic_exchange_n(&g_menu_closed_pending, 0, __ATOMIC_ACQ_REL)) {
            int m = __atomic_load_n(&g_menu_closed_count, __ATOMIC_ACQUIRE);
            for (int i = 0; i < m; ++i) g_menu_closed[i].fn(g_menu_closed[i].user);
        }
        fefw_nx::settings_flush();
    }
    __atomic_store_n(&g_tick_busy, 0, __ATOMIC_RELEASE);
}

// nn::hid::Npad*State: { s64 sampling_number; NpadButtonSet buttons; AnalogStick l, r; attributes; }
void post_npad(void* state) {
    frame_tick();
    if (__atomic_load_n(&g_inject_frames, __ATOMIC_ACQUIRE) && state) {
        reinterpret_cast<uint64_t*>(state)[1] |= __atomic_load_n(&g_inject_buttons, __ATOMIC_ACQUIRE);
    }
}

// ------------------------------------------------------------------------------------------------
// Menu service: native ConfigMenuItem with a cloned vtable whose text/confirm slots point here.
// ------------------------------------------------------------------------------------------------
// Layout: native header, self pointer at +0xA0, then our data.
struct PluginMenuItem {
    uint8_t native[0xA0];          // engine ConfigMenuItem header
    PluginMenuItem* owner;         // 0xA0: always self
    const FefwMenuEntry* entry;    // 0xA8
    uint64_t table[kUiItemVtableBytes / 8];
    FefwMenuStepFn step;           // Left / Right, set by menu_set_step
};
static_assert(__builtin_offsetof(PluginMenuItem, owner) == 0xA0, "native ConfigMenuItem prefix");
static_assert(sizeof(PluginMenuItem) <= 0x300, "menu items are allocated as 0x300 bytes");

const char* item_text(PluginMenuItem* item) {
    const FefwMenuEntry* e = item->entry;
    const char* s = (e && e->text) ? e->text(e->user, item) : nullptr;
    return s ? s : "";
}

int item_confirm(PluginMenuItem* item) {
    if (item->native[0x52] != 1) return FEFW_MENU_IGNORED;  // row disabled
    const FefwMenuEntry* e = item->entry;
    return (e && e->confirm) ? e->confirm(e->user, item) : FEFW_MENU_IGNORED;
}

// Left / Right slots: (item, bool) -> 1 when the value changed, else 0 (as the game's own setting rows).
int item_step(PluginMenuItem* item, int dir) {
    if (item->native[0x52] != 1 || !item->step) return 0;
    const FefwMenuEntry* e = item->entry;
    return item->step(e ? e->user : nullptr, item, dir) ? 1 : 0;
}
int item_right(PluginMenuItem* item, int) { return item_step(item, 1); }
int item_left(PluginMenuItem* item, int) { return item_step(item, -1); }

int menu_set_step(void* item, FefwMenuStepFn fn) {
    if (!item) return -1;
    static_cast<PluginMenuItem*>(item)->step = fn;
    return 0;
}

// Destructor slots 0 (complete) and 1 (deleting) of the cloned vtable: count the row out, then run the game's own.
using ItemDtor = void (*)(PluginMenuItem*);
ItemDtor g_item_dtor[2];

void row_destroyed() {
    if (__atomic_sub_fetch(&g_live_rows, 1, __ATOMIC_ACQ_REL) == 0)
        __atomic_store_n(&g_menu_closed_pending, 1, __ATOMIC_RELEASE);
}

void item_dtor_complete(PluginMenuItem* item) {
    row_destroyed();
    g_item_dtor[0](item);
}

void item_dtor_deleting(PluginMenuItem* item) {
    row_destroyed();
    g_item_dtor[1](item);  // frees `item`
}

void* menu_add_entry(void* menu, const FefwMenuEntry* entry) {
    if (!menu || !entry || !g_menu_ok) return nullptr;
    void* heap = **reinterpret_cast<void***>(main_base() + kUiHeapGot);
    auto alloc = game_fn<void* (*)(void*, size_t, unsigned, unsigned)>(kUiAlloc);
    auto* item = static_cast<PluginMenuItem*>(alloc(heap, sizeof(PluginMenuItem), 16, 0));
    if (!item) return nullptr;
    memset(item->native, 0, sizeof(item->native));
    memcpy(item->table, reinterpret_cast<const void*>(main_base() + kUiItemVtablePrefix), kUiItemVtableBytes);
    uint64_t* vt = item->table + 2;  // skip offset-to-top + typeinfo
    vt[kVtText / 8] = reinterpret_cast<uint64_t>(&item_text);
    vt[kVtConfirm / 8] = reinterpret_cast<uint64_t>(&item_confirm);
    vt[kVtRight / 8] = reinterpret_cast<uint64_t>(&item_right);
    vt[kVtLeft / 8] = reinterpret_cast<uint64_t>(&item_left);
    g_item_dtor[0] = reinterpret_cast<ItemDtor>(vt[0]);
    g_item_dtor[1] = reinterpret_cast<ItemDtor>(vt[1]);
    vt[0] = reinterpret_cast<uint64_t>(&item_dtor_complete);
    vt[1] = reinterpret_cast<uint64_t>(&item_dtor_deleting);
    __atomic_add_fetch(&g_live_rows, 1, __ATOMIC_ACQ_REL);
    *reinterpret_cast<uint64_t**>(item->native) = vt;
    *reinterpret_cast<uint32_t*>(item->native + 0x50) = 0x00010101;  // visible / enabled flags
    item->owner = item;
    item->entry = entry;
    item->step = nullptr;
    game_fn<void (*)(void*, void*)>(kUiAddItem)(menu, item);
    return item;
}

void* menu_open_submenu(void* item) {
    if (!item || !g_menu_ok) return nullptr;
    void* parent = *reinterpret_cast<void**>(static_cast<uint8_t*>(item) + 8);
    if (parent) {
        void* bound = *reinterpret_cast<void**>(static_cast<uint8_t*>(parent) + 0x98);
        if (bound) parent = bound;
    }
    if (!parent) return nullptr;
    uint64_t info[3] = {reinterpret_cast<uint64_t>(item), 1, 0};
    return game_fn<void* (*)(void*, const uint64_t*)>(kUiSubmenu)(parent, info);
}

void menu_refresh(void* item) {
    if (item && g_menu_ok) game_fn<void (*)(void*)>(kUiRefresh)(item);
}

// ------------------------------------------------------------------------------------------------
// Plugin host
// ------------------------------------------------------------------------------------------------
void api_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fefw_nx::vlog(fmt, ap);
    va_end(ap);
}

int api_is_readable(uintptr_t addr, size_t size) { return fefw_nx::is_readable(addr, size) ? 1 : 0; }

char g_language[8] = "";

const char* api_get_language() { return g_language; }

uint64_t api_call_game(uintptr_t addr, const uint64_t* args, int nargs) {
    uint64_t a[8] = {};
    for (int i = 0; i < nargs && i < 8; ++i) a[i] = args[i];
    // AAPCS64: unused argument registers are ignored by the callee
    using Fn = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    return reinterpret_cast<Fn>(addr)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}

FefwApi g_api{};

using Result = uint32_t;
void* alloc_pages(size_t size) {
    size = (size + 0xFFF) & ~size_t(0xFFF);
    auto raw = reinterpret_cast<uintptr_t>(malloc(size + 0x1000));
    if (!raw) return nullptr;
    void* p = reinterpret_cast<void*>((raw + 0xFFF) & ~uintptr_t(0xFFF));
    memset(p, 0, size);
    return p;
}

struct ListCtx {
    char names[16][64];
    int count;
};

void collect_nro(void* ctx, const char* name) {
    auto* c = static_cast<ListCtx*>(ctx);
    size_t n = strlen(name);
    if (c->count < 16 && n > 4 && n < 60 && strcmp(name + n - 4, ".nro") == 0) {
        memcpy(c->names[c->count], name, n - 4);
        c->names[c->count][n - 4] = 0;
        ++c->count;
    }
}

}  // namespace

void fefw_nx::note_game_file(const char* vpath) {
    if (!vpath || strncmp(vpath, "/Msg/", 5) != 0) return;
    const char* lang = vpath + 5;
    const char* slash = strchr(lang, '/');
    size_t n = slash ? static_cast<size_t>(slash - lang) : 0;
    if (n == 0 || n >= sizeof(g_language) || (strncmp(g_language, lang, n) == 0 && g_language[n] == 0)) return;
    char tmp[sizeof(g_language)] = {};
    memcpy(tmp, lang, n);
    memcpy(g_language, tmp, sizeof(g_language));  // whole-buffer copy: readers never see a half-written code
    log("game language: %s", g_language);
}

void fefw_nx::load_plugins(const char* mount, PluginReadFn read_file, PluginListFn list_dir) {
    char dir[96];
    snprintf(dir, sizeof(dir), "%s:/fefw/plugins", mount);
    static ListCtx list;  // large: keep off the (game) stack
    list.count = 0;
    if (!list_dir(dir, &collect_nro, &list) || list.count == 0) return;

    register_mid_hook("system_menu", &on_system_menu, nullptr);
    g_api.version = FEFW_PLUGIN_API_VERSION;
    g_api.size = sizeof(FefwApi);
    g_api.main_base = main_base();
    g_api.log = &api_log;
    g_api.register_system_menu_provider = &register_system_menu_provider;
    g_api.register_frame_callback = &register_frame_callback;
    g_api.inject_buttons = &inject_buttons;
    g_api.menu_add_entry = &menu_add_entry;
    g_api.menu_open_submenu = &menu_open_submenu;
    g_api.menu_refresh = &menu_refresh;
    g_api.is_readable = &api_is_readable;
    g_api.register_mid_hook = &register_mid_hook;
    g_api.get_language = &api_get_language;
    g_api.call_game = &api_call_game;
    g_api.register_menu_closed_callback = &register_menu_closed_callback;
    g_api.call_game_sret = &api_call_game_sret;
    g_api.settings_get = &fefw_nx::settings_get;
    g_api.settings_set = &fefw_nx::settings_set;
    g_api.menu_set_step = &menu_set_step;
    g_api.register_menu_group_provider = &register_menu_group_provider;

#if FEFW_NX_NRO
    auto ro_init = static_cast<Result (*)()>(nullptr);
    auto ro_register = reinterpret_cast<Result (*)(void*, const void*)>(&nn::ro::RegisterModuleInfo);
    auto ro_bufsize = reinterpret_cast<Result (*)(size_t*, const void*)>(&nn::ro::GetBufferSize);
    auto ro_load = reinterpret_cast<Result (*)(void*, const void*, void*, size_t, int)>(&nn::ro::LoadModule);
    auto ro_lookup = reinterpret_cast<Result (*)(uintptr_t*, const void*, const char*)>(&nn::ro::LookupModuleSymbol);
    if (!ro_register || !ro_bufsize || !ro_load || !ro_lookup) {
        log("plugins: nn::ro entry points not found");
        return;
    }
#else
    auto ro_init = reinterpret_cast<Result (*)()>(resolve_sdk_symbol("_ZN2nn2ro10InitializeEv"));
    auto ro_register = reinterpret_cast<Result (*)(void*, const void*)>(
        resolve_sdk_symbol("_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv"));
    auto ro_bufsize = reinterpret_cast<Result (*)(size_t*, const void*)>(
        resolve_sdk_symbol("_ZN2nn2ro13GetBufferSizeEPmPKv"));
    auto ro_load = reinterpret_cast<Result (*)(void*, const void*, void*, size_t, int)>(
        resolve_sdk_symbol("_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi"));
    auto ro_lookup = reinterpret_cast<Result (*)(uintptr_t*, const void*, const char*)>(
        resolve_sdk_symbol("_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc"));
    if (!ro_init || !ro_register || !ro_bufsize || !ro_load || !ro_lookup) {
        log("plugins: nn::ro entry points not found");
        return;
    }
    ro_init();  // static loader only; the NRO bootstrap already initialized nn::ro
#endif

    for (int i = 0; i < list.count; ++i) {
        const char* name = list.names[i];
        char path[160];
        void* nro = nullptr;
        void* nrr = nullptr;
        size_t nro_size = 0, nrr_size = 0;
        snprintf(path, sizeof(path), "%s/%s.nro", dir, name);
        bool ok = read_file(path, &nro, &nro_size);
        snprintf(path, sizeof(path), "%s/%s.nrr", dir, name);
        ok = ok && read_file(path, &nrr, &nrr_size);
        if (!ok) {
            log("plugin %s: missing .nro or .nrr", name);
            continue;
        }
        void* reg = alloc_pages(0x1000);
        void* mod = alloc_pages(0x1000);
        Result r = ro_register(reg, nrr);
        size_t bss_size = 0;
        if (r == 0) r = ro_bufsize(&bss_size, nro);
        void* bss = (r == 0 && bss_size) ? alloc_pages(bss_size) : nullptr;
        if (r == 0) r = ro_load(mod, nro, bss, bss_size, 1 /* BindFlag_Now */);
        uintptr_t entry = 0;
        if (r == 0) r = ro_lookup(&entry, mod, "fefw_plugin_main");
        if (r != 0 || !entry) {
            log("plugin %s: load failed (0x%x)", name, r);
            continue;
        }
        int rc = reinterpret_cast<FefwPluginMainFn>(entry)(&g_api);
        log("plugin %s: loaded, fefw_plugin_main -> %d", name, rc);
    }
}

// ------------------------------------------------------------------------------------------------
// Bootstrap entry points (looked up by name with nn::ro::LookupSymbol, see nx/boot/boot.c)
// ------------------------------------------------------------------------------------------------
extern "C" {

#if FEFW_NX_NRO
FEFW_EXPORT void fefw_hid_GetNpadStateHandheld(nn::hid::NpadHandheldState* s, const unsigned int* id) {
    nn::hid::GetNpadState(s, *id);
    post_npad(s);
}
FEFW_EXPORT void fefw_hid_GetNpadStateJoyDual(nn::hid::NpadJoyDualState* s, const unsigned int* id) {
    nn::hid::GetNpadState(s, *id);
    post_npad(s);
}
FEFW_EXPORT void fefw_hid_GetNpadStateFullKey(nn::hid::NpadFullKeyState* s, const unsigned int* id) {
    nn::hid::GetNpadState(s, *id);
    post_npad(s);
}
#endif

// Mid-function hook entry points, one per profile slot (the bootstrap looks them up by name).
#define FEFW_MID_ENTRY(k) \
    FEFW_EXPORT void fefw_mid_##k(uint64_t* ctx) { if ((k) < kMidHookCount) run_mid_hooks((k), ctx); }
FEFW_MID_ENTRY(0) FEFW_MID_ENTRY(1) FEFW_MID_ENTRY(2) FEFW_MID_ENTRY(3) FEFW_MID_ENTRY(4) FEFW_MID_ENTRY(5) FEFW_MID_ENTRY(6) FEFW_MID_ENTRY(7)
FEFW_MID_ENTRY(8) FEFW_MID_ENTRY(9) FEFW_MID_ENTRY(10) FEFW_MID_ENTRY(11) FEFW_MID_ENTRY(12) FEFW_MID_ENTRY(13) FEFW_MID_ENTRY(14) FEFW_MID_ENTRY(15)
FEFW_MID_ENTRY(16) FEFW_MID_ENTRY(17) FEFW_MID_ENTRY(18) FEFW_MID_ENTRY(19) FEFW_MID_ENTRY(20) FEFW_MID_ENTRY(21) FEFW_MID_ENTRY(22) FEFW_MID_ENTRY(23)
FEFW_MID_ENTRY(24) FEFW_MID_ENTRY(25) FEFW_MID_ENTRY(26) FEFW_MID_ENTRY(27) FEFW_MID_ENTRY(28) FEFW_MID_ENTRY(29) FEFW_MID_ENTRY(30) FEFW_MID_ENTRY(31)
FEFW_MID_ENTRY(32) FEFW_MID_ENTRY(33) FEFW_MID_ENTRY(34) FEFW_MID_ENTRY(35) FEFW_MID_ENTRY(36) FEFW_MID_ENTRY(37) FEFW_MID_ENTRY(38) FEFW_MID_ENTRY(39)
FEFW_MID_ENTRY(40) FEFW_MID_ENTRY(41) FEFW_MID_ENTRY(42) FEFW_MID_ENTRY(43) FEFW_MID_ENTRY(44) FEFW_MID_ENTRY(45) FEFW_MID_ENTRY(46) FEFW_MID_ENTRY(47)
FEFW_MID_ENTRY(48) FEFW_MID_ENTRY(49) FEFW_MID_ENTRY(50) FEFW_MID_ENTRY(51) FEFW_MID_ENTRY(52) FEFW_MID_ENTRY(53) FEFW_MID_ENTRY(54) FEFW_MID_ENTRY(55)
FEFW_MID_ENTRY(56) FEFW_MID_ENTRY(57) FEFW_MID_ENTRY(58) FEFW_MID_ENTRY(59) FEFW_MID_ENTRY(60) FEFW_MID_ENTRY(61) FEFW_MID_ENTRY(62) FEFW_MID_ENTRY(63)
#undef FEFW_MID_ENTRY

}  // extern "C"
