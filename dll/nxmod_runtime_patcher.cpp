// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher as a code mod (DLL) for PC recompilation runtimes.
//
// Install: <mods>/FEFW_RuntimePatcher/exefs/fefw_runtime_patcher.dll.
//   * nxmod_early: scans every installed mod's romfs/ for RuntimePatcher packages and registers VFS overlay edits.
//   * fefw_nxmod_host_attach: provides the single process-wide FefwApi host (shared MenuHost + SettingsStore, mid-hook
//     dispatcher, frame tick, and guest-call context) so code-plugin DLLs do not duplicate the host or use IPC.
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "fefw_foundation/plugin_host_core.h"
#include "fefw_foundation/vfs_overlay.h"
#include "nxmod_abi.h"
#include "nxmod_host_abi.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define NXMOD_EXPORT extern "C" __declspec(dllexport)
#else
#include <dirent.h>
#include <sys/stat.h>
#define NXMOD_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

// ------------------------------------------------------------------------------------------------
// VFS Overlay (nxmod_early)
// ------------------------------------------------------------------------------------------------
const NxModApi* g_api = nullptr;
fefw_foundation::VfsOverlayPlan* g_plan = nullptr;
std::deque<std::string> g_targets;  // stable storage: the user pointer of an edit is its virtual path

void logv(int level, const char* fmt, va_list ap) {
    if (!g_api || !g_api->log) return;
    char buf[1024];
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    g_api->log(g_api, level, buf);
}

void logf(int level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logv(level, fmt, ap);
    va_end(ap);
}

bool host_read(const std::string& path, std::vector<uint8_t>& out) {
    const int64_t n = g_api->read_file(path.c_str(), nullptr, 0);
    if (n < 0) return false;
    out.resize(static_cast<size_t>(n));
    if (n == 0) return true;
    return g_api->read_file(path.c_str(), out.data(), out.size()) == n;
}

// The runtime offers no directory listing, so folders are listed with the OS.
#ifdef _WIN32
std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
bool host_list(const std::string& dir, std::vector<fefw_foundation::HostDirEntry>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((widen(dir) + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        out.push_back({narrow(fd.cFileName), (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}
#else
bool host_list(const std::string& dir, std::vector<fefw_foundation::HostDirEntry>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        struct stat st {};
        bool is_dir = stat((dir + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        out.push_back({name, is_dir});
    }
    closedir(d);
    return true;
}
#endif

void on_edit(const NxModFile* file, void* user) {
    const std::string& vpath = *static_cast<const std::string*>(user);
    auto read_base = [&](const std::string& vp, std::vector<uint8_t>& out) {
        if (vp != vpath || !file->data) return false;
        out.assign(file->data, file->data + file->size);
        return true;
    };
    std::vector<uint8_t> out;
    std::string err;
    if (fefw_foundation::synthesize_vfs_file(*g_plan, vpath, read_base, out, &err)) {
        file->replace(file, out.data(), out.size());
        logf(2, "patched %s (%llu -> %zu bytes)", vpath.c_str(), static_cast<unsigned long long>(file->size),
             out.size());
    } else {
        logf(1, "%s left as it is (%s)", vpath.c_str(), err.empty() ? "nothing to apply" : err.c_str());
    }
}

// ------------------------------------------------------------------------------------------------
// Singleton FefwApi plugin host (shared by all PC recompilation plugin DLLs in the process)
// ------------------------------------------------------------------------------------------------
const NxModApi* g_host_nx = nullptr;
uint64_t g_main = 0;
uint64_t g_system_menu_reg = 0;
bool g_host_initialized = false;
bool g_on_frame_registered = false;
bool g_system_menu_hooked = false;
FefwApi g_fefw_api{};
fefw_foundation::MenuHost g_menu_host;
fefw_foundation::ServiceRegistry g_services;
std::string g_settings_path;
std::chrono::steady_clock::time_point g_t0;

thread_local NxCtx* t_ctx = nullptr;  // guest CPU context of the active runtime callback

struct CtxScope {
    NxCtx* saved;
    explicit CtxScope(NxCtx* c) : saved(t_ctx) { t_ctx = c; }
    ~CtxScope() { t_ctx = saved; }
};

void set_settings_path_from_mod_dir(const char* mod_dir) {
    if (!g_settings_path.empty() || !mod_dir || !mod_dir[0]) return;
    std::string dir(mod_dir);
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) {
        g_settings_path = dir.substr(0, slash) + "/fefw_settings.ini";
    }
}

void api_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logv(2, fmt, ap);
    va_end(ap);
}

uint64_t api_call_game(uintptr_t addr, const uint64_t* args, int nargs) {
    if (!t_ctx) {
        logf(1, "call_game(0x%llx) outside a runtime callback: not called", static_cast<unsigned long long>(addr));
        return 0;
    }
    return g_host_nx->call_guest(t_ctx, addr, args, nargs);
}

uint64_t api_call_game_sret(uintptr_t addr, void* result, const uint64_t* args, int nargs) {
    if (!t_ctx) {
        logf(1, "call_game_sret(0x%llx) outside a runtime callback: not called",
             static_cast<unsigned long long>(addr));
        return 0;
    }
    uint64_t saved = NxModRegs(t_ctx)[8];
    NxModRegs(t_ctx)[8] = reinterpret_cast<uint64_t>(result);
    uint64_t r = g_host_nx->call_guest(t_ctx, addr, args, nargs);
    NxModRegs(t_ctx)[8] = saved;
    return r;
}

int api_is_readable(uintptr_t addr, size_t size) { return addr && g_host_nx->is_mapped(addr, size) ? 1 : 0; }

const char* api_get_language() { return ""; }  // not tracked here: plugins fall back to English

void api_inject_buttons(uint64_t, uint32_t) {}  // not supported on this host

// ---- mid hooks ------------------------------------------------------------------------------------------------------
struct StoredSite {
    std::string name;
    uint64_t va;
    int entry;
};
std::vector<StoredSite> g_sites;

void merge_sites(const FefwHostSite* sites, int count) {
    if (!sites || count <= 0) return;
    for (int i = 0; i < count; ++i) {
        if (!sites[i].name || !sites[i].name[0]) continue;
        bool exists = false;
        for (const auto& s : g_sites) {
            if (s.va == sites[i].va && s.entry == sites[i].entry && s.name == sites[i].name) {
                exists = true;
                break;
            }
        }
        if (!exists) g_sites.push_back({sites[i].name, sites[i].va, sites[i].entry});
    }
}

constexpr int kMaxMidHooks = 256;
struct MidHook {
    FefwMidHookFn fn;
    void* user;
};
MidHook g_mid[kMaxMidHooks];
int g_mid_count = 0;

// call_guest keeps the caller's x30, so a game call a hook makes can reach the same hook again.
// Skip a hook that is already running on this thread to prevent infinite recursion.
thread_local bool t_mid_busy[kMaxMidHooks];

int mid_trampoline(NxCtx* c, uint64_t, void* user) {
    auto* h = static_cast<MidHook*>(user);
    bool& busy = t_mid_busy[h - g_mid];
    if (busy) return 0;
    busy = true;
    CtxScope scope(c);
    h->fn(h->user, NxModRegs(c));
    busy = false;
    return 0;  // the instruction at the site runs as usual
}

int api_register_mid_hook(const char* name, FefwMidHookFn fn, void* user) {
    if (!name || !fn) return 0;
    if (g_mid_count >= kMaxMidHooks) {
        logf(1, "mid hook limit (%d) reached; '%s' was not registered", kMaxMidHooks, name);
        return 0;
    }
    MidHook* h = &g_mid[g_mid_count];
    *h = {fn, user};
    int placed = 0;
    for (const auto& s : g_sites) {
        if (s.name != name) continue;
        uint64_t addr = g_main + s.va;
        if (!g_host_nx->is_function(addr)) {
            logf(1, "hook site %s (0x%llx) was not lifted: run Set up with this mod installed", name,
                 static_cast<unsigned long long>(addr));
            continue;
        }
        uint32_t id = s.entry ? g_host_nx->hook_entry(addr, &mid_trampoline, h)
                              : g_host_nx->hook_mid(addr, &mid_trampoline, h);
        if (id) ++placed;
    }
    if (placed) ++g_mid_count;
    return placed;
}

// ---- frame callbacks ------------------------------------------------------------------------------------------------
constexpr int kMaxFrameCallbacks = 32;
struct FrameCb {
    FefwFrameFn fn;
    void* user;
};
FrameCb g_frame_cbs[kMaxFrameCallbacks];
int g_frame_cb_count = 0;

void on_frame(NxCtx* c, void*) {
    CtxScope scope(c);
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_t0);
    uint64_t frame = static_cast<uint64_t>(elapsed_us.count() * 125 / 1000000) + 1;
    for (int i = 0; i < g_frame_cb_count; ++i) g_frame_cbs[i].fn(g_frame_cbs[i].user, frame);
    g_menu_host.flush_menu_closed();
}

void ensure_on_frame() {
    if (!g_on_frame_registered && g_host_nx) {
        g_host_nx->on_frame(&on_frame, nullptr);
        g_on_frame_registered = true;
    }
}

int api_register_frame_callback(FefwFrameFn fn, void* user) {
    if (!fn || g_frame_cb_count >= kMaxFrameCallbacks) return 0;
    g_frame_cbs[g_frame_cb_count++] = {fn, user};
    ensure_on_frame();
    return 1;
}

// ---- System menu service (shared MenuHost + make_function vtable thunks) --------------------------------------------
void slot_text(NxCtx* c, void*) {
    CtxScope scope(c);
    auto* item = reinterpret_cast<fefw_foundation::PluginMenuItem*>(NxModRegs(c)[0]);
    NxModRegs(c)[0] = reinterpret_cast<uint64_t>(fefw_foundation::MenuHost::handle_text(item));
}

void slot_confirm(NxCtx* c, void*) {
    CtxScope scope(c);
    auto* item = reinterpret_cast<fefw_foundation::PluginMenuItem*>(NxModRegs(c)[0]);
    NxModRegs(c)[0] = static_cast<uint32_t>(fefw_foundation::MenuHost::handle_confirm(item));
}

void slot_step(NxCtx* c, void* dir_p) {
    CtxScope scope(c);
    auto* item = reinterpret_cast<fefw_foundation::PluginMenuItem*>(NxModRegs(c)[0]);
    int dir = static_cast<int>(reinterpret_cast<intptr_t>(dir_p));
    NxModRegs(c)[0] = static_cast<uint32_t>(fefw_foundation::MenuHost::handle_step(item, dir));
}

void slot_dtor(NxCtx* c, void* which) {
    CtxScope scope(c);
    auto* item = reinterpret_cast<fefw_foundation::PluginMenuItem*>(NxModRegs(c)[0]);
    int k = static_cast<int>(reinterpret_cast<intptr_t>(which));
    g_menu_host.handle_dtor(item, k);
}

void on_system_menu(void*, uint64_t* regs) {
    g_menu_host.on_system_menu(reinterpret_cast<void*>(regs[g_system_menu_reg]));
}

bool ensure_system_menu_hook() {
    if (!g_menu_host.is_ready()) return false;
    if (g_system_menu_hooked) return true;
    if (!api_register_mid_hook("system_menu", &on_system_menu, nullptr)) {
        logf(1, "no System menu: its hook site was not lifted");
        g_menu_host.disable();
        return false;
    }
    g_system_menu_hooked = true;
    return true;
}

int api_register_system_menu_provider(FefwMenuProviderFn fn, void* user) {
    if (!fn || !ensure_system_menu_hook()) return 0;
    return g_menu_host.register_system_menu_provider(fn, user);
}

int api_register_menu_group_provider(const char* group_name, FefwMenuProviderFn fn, void* user) {
    if (!ensure_system_menu_hook()) return 0;
    return g_menu_host.register_menu_group_provider(group_name, fn, user);
}

int api_register_menu_group_provider_ordered(const char* group_name, int order, FefwMenuProviderFn fn, void* user) {
    if (!ensure_system_menu_hook()) return 0;
    return g_menu_host.register_menu_group_provider_ordered(group_name, order, fn, user);
}

int api_register_menu_subgroup_provider(const char* group_name, const char* sub_id, int sub_order,
                                        const char* (*title_fn)(void* user, void* item), void* title_user,
                                        int item_order, const char* item_key, FefwMenuProviderFn fn, void* user) {
    if (!ensure_system_menu_hook()) return 0;
    return g_menu_host.register_menu_subgroup_provider(group_name, sub_id, sub_order, title_fn, title_user,
                                                       item_order, item_key, fn, user);
}

int api_register_menu_closed_callback(FefwMenuClosedFn fn, void* user) {
    if (!fn || !g_menu_host.is_ready()) return 0;
    int ok = g_menu_host.register_menu_closed_callback(fn, user);
    if (ok) ensure_on_frame();
    return ok;
}

void* api_menu_add_entry(void* menu, const FefwMenuEntry* entry) { return g_menu_host.add_entry(menu, entry); }
void* api_menu_open_submenu(void* item) { return g_menu_host.open_submenu(item); }
void api_menu_refresh(void* item) { g_menu_host.refresh(item); }
int api_menu_set_step(void* item, FefwMenuStepFn fn) { return fefw_foundation::MenuHost::set_step(item, fn); }
int api_register_service(const char* name, void* ptr) { return g_services.register_service(name, ptr); }
void* api_get_service(const char* name) { return g_services.get_service(name); }

// ---- settings: <mods>/fefw_settings.ini via shared SettingsStore ----------------------------------------------------
void load_settings_file(fefw_foundation::SettingsStore& store) {
    store.clear();
    if (g_settings_path.empty()) return;
    FILE* f = std::fopen(g_settings_path.c_str(), "rb");
    if (!f) return;
    static char buf[fefw_foundation::SettingsStore::kMaxIniBytes];
    size_t got = std::fread(buf, 1, sizeof(buf), f);
    std::fclose(f);
    store.parse_ini(buf, got);
}

int api_settings_get(const char* key, char* out, size_t out_size) {
    if (!key || !out || !out_size) return -1;
    static fefw_foundation::SettingsStore store;
    load_settings_file(store);
    return store.get(key, out, out_size);
}

int api_settings_set(const char* key, const char* value) {
    if (!key || !value) return -1;
    static fefw_foundation::SettingsStore store;
    load_settings_file(store);
    if (store.set(key, value) != 0) return -1;
    if (g_settings_path.empty()) return 0;
    static char buf[fefw_foundation::SettingsStore::kMaxIniBytes];
    size_t n = store.format_ini(buf, sizeof(buf));
    std::string tmp = g_settings_path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        logf(1, "settings: cannot write %s", tmp.c_str());
        return 0;
    }
    if (n) std::fwrite(buf, 1, n, f);
    std::fclose(f);
#ifdef _WIN32
    if (!MoveFileExA(tmp.c_str(), g_settings_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        logf(1, "settings: cannot replace %s", g_settings_path.c_str());
    }
#else
    if (std::rename(tmp.c_str(), g_settings_path.c_str()) != 0) {
        logf(1, "settings: cannot replace %s", g_settings_path.c_str());
    }
#endif
    return 0;
}

}  // namespace

NXMOD_EXPORT int nxmod_early(const NxModApi* api, const char* mod_dir) {
    set_settings_path_from_mod_dir(mod_dir);
    if (!api || api->version < NXMOD_MIN_VERSION || api->size < sizeof(NxModApi)) return 1;
    g_api = api;
    logf(2, "FEFW RuntimePatcher %s", FEFW_VERSION);

    std::vector<std::string> overlays;
    for (uint32_t i = 0, n = api->mod_count(); i < n; ++i) {
        if (const char* f = api->mod_folder(i)) overlays.push_back(std::string(f) + "/romfs");
    }
    fefw_foundation::HostFs fs;
    fs.read_file = host_read;
    fs.list_dir = host_list;

    g_plan = new fefw_foundation::VfsOverlayPlan();
    std::string err;
    if (!fefw_foundation::plan_vfs_overlays(overlays, fs, *g_plan, &err)) {
        logf(0, "planning failed: %s", err.empty() ? "unknown error" : err.c_str());
        return 1;
    }
    for (const auto& id : g_plan->applied_mods) logf(2, "  mod: %s", id.c_str());
    if (g_plan->applied_mods.empty()) {
        logf(2, "no RuntimePatcher mods installed");
        return 0;
    }
    for (const auto& vpath : g_plan->target_paths) {
        g_targets.push_back(vpath);
        if (!api->edit_file(vpath.c_str() + (vpath[0] == '/' ? 1 : 0), &on_edit, &g_targets.back())) {
            logf(1, "cannot edit %s", vpath.c_str());
        }
    }
    logf(2, "%zu mod(s), %zu file(s) to patch", g_plan->applied_mods.size(), g_targets.size());
    return 0;
}

// Required: a DLL without nxmod_init is unloaded.
NXMOD_EXPORT int nxmod_init(const NxModApi* api, const char* mod_dir) {
    set_settings_path_from_mod_dir(mod_dir);
    if (api) {
        if (!g_api) g_api = api;
        if (!g_host_nx) g_host_nx = api;
    }
    return 0;
}

// Single process-wide FefwApi host entry point called by every code-plugin DLL's nxmod_init().
NXMOD_EXPORT const FefwApi* fefw_nxmod_host_attach(const NxModApi* nx_api, const char* mod_dir,
                                                   const FefwHostGameConfig* game_cfg, const FefwHostSite* sites,
                                                   int site_count) {
    if (!nx_api || nx_api->version < 2 || nx_api->size < offsetof(NxModApi, edit_file)) return nullptr;
    if (!game_cfg || game_cfg->abi_version != FEFW_NXMOD_HOST_ABI_VERSION ||
        game_cfg->size < sizeof(FefwHostGameConfig)) {
        return nullptr;
    }
    set_settings_path_from_mod_dir(mod_dir);
    merge_sites(sites, site_count);

    if (!g_host_initialized) {
        g_host_nx = nx_api;
        if (!g_api) g_api = nx_api;
        g_main = nx_api->main_base;
        g_system_menu_reg = game_cfg->system_menu_reg;
        g_t0 = std::chrono::steady_clock::now();

        bool menu_ok = nx_api->is_mapped(g_main + game_cfg->menu_resume, 4) &&
                       *reinterpret_cast<const uint32_t*>(g_main + game_cfg->menu_resume) == game_cfg->menu_resume_insn;
        if (menu_ok) {
            fefw_foundation::MenuPlatform plat{};
            plat.main_base = static_cast<uintptr_t>(g_main);
            plat.call_game = &api_call_game;
            plat.slot_text = nx_api->make_function(&slot_text, nullptr);
            plat.slot_confirm = nx_api->make_function(&slot_confirm, nullptr);
            plat.slot_left = nx_api->make_function(&slot_step, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
            plat.slot_right = nx_api->make_function(&slot_step, reinterpret_cast<void*>(static_cast<intptr_t>(1)));
            plat.slot_dtor[0] = nx_api->make_function(&slot_dtor, reinterpret_cast<void*>(0));
            plat.slot_dtor[1] = nx_api->make_function(&slot_dtor, reinterpret_cast<void*>(1));
            menu_ok = g_menu_host.init(game_cfg->menu, plat);
        }
        if (!menu_ok) logf(1, "menu service off (unknown game build or no function slots)");

        g_fefw_api.version = FEFW_PLUGIN_API_VERSION;
        g_fefw_api.size = sizeof(FefwApi);
        g_fefw_api.main_base = static_cast<uintptr_t>(g_main);
        g_fefw_api.log = &api_log;
        g_fefw_api.register_system_menu_provider = &api_register_system_menu_provider;
        g_fefw_api.register_frame_callback = &api_register_frame_callback;
        g_fefw_api.inject_buttons = &api_inject_buttons;
        g_fefw_api.menu_add_entry = &api_menu_add_entry;
        g_fefw_api.menu_open_submenu = &api_menu_open_submenu;
        g_fefw_api.menu_refresh = &api_menu_refresh;
        g_fefw_api.is_readable = &api_is_readable;
        g_fefw_api.register_mid_hook = &api_register_mid_hook;
        g_fefw_api.get_language = &api_get_language;
        g_fefw_api.call_game = &api_call_game;
        g_fefw_api.register_menu_closed_callback = &api_register_menu_closed_callback;
        g_fefw_api.call_game_sret = &api_call_game_sret;
        g_fefw_api.settings_get = &api_settings_get;
        g_fefw_api.settings_set = &api_settings_set;
        g_fefw_api.menu_set_step = &api_menu_set_step;
        g_fefw_api.register_menu_group_provider = &api_register_menu_group_provider;
        g_fefw_api.register_menu_group_provider_ordered = &api_register_menu_group_provider_ordered;
        g_fefw_api.register_menu_subgroup_provider = &api_register_menu_subgroup_provider;
        g_fefw_api.register_service = &api_register_service;
        g_fefw_api.get_service = &api_get_service;

        g_host_initialized = true;
    }
    return &g_fefw_api;
}
