// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — platform-independent plugin host core (System menu service + settings store).
//
// Shared by both the Switch NRO runtime (nx/src/nx_game_hooks.cpp, nx/src/nx_settings.cpp) and the PC recompilation
// host (dll/nxmod_runtime_patcher.cpp). Contains no game addresses, no OS headers, and no platform-specific hooks.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "fefw_foundation/plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Game-specific UI offsets/sizes supplied by the platform host (from game profile / hooks_gen.h).
typedef struct FefwMenuConfig {
    uint64_t ui_heap_got;
    uint64_t ui_alloc;
    uint64_t ui_submenu;
    uint64_t ui_add_item;
    uint64_t ui_refresh;
    uint64_t ui_item_vtable;
    size_t ui_item_vtable_bytes;
    size_t vt_text;
    size_t vt_confirm;
    size_t vt_right;
    size_t vt_left;
} FefwMenuConfig;

#ifdef __cplusplus
}  // extern "C"

namespace fefw_foundation {

// ------------------------------------------------------------------------------------------------
// SettingsStore: in-memory key=value store + INI parser/formatter shared by Switch & PC hosts.
// ------------------------------------------------------------------------------------------------
class SettingsStore {
public:
    static constexpr int kMaxEntries = 64;
    static constexpr int kKeyLen = 64;
    static constexpr int kValueLen = 128;
    static constexpr size_t kMaxIniBytes = kMaxEntries * (kKeyLen + kValueLen + 2);

    struct Entry {
        char key[kKeyLen];
        char value[kValueLen];
    };

    void clear();
    int count() const { return count_; }
    const Entry* entries() const { return entries_; }

    // Copies the value (NUL-terminated, truncated to out_size) and returns its length, or -1 if missing.
    int get(const char* key, char* out, size_t out_size) const;

    // Validates and stores key=value. Returns 0 on success, -1 on invalid key/value or full table.
    int set(const char* key, const char* value);

    // Low-level insert/update without validation (used by INI parsing and merge).
    bool put(const char* key, size_t klen, const char* value, size_t vlen);

    // Parses `key=value` lines (ignoring `#` comments and blank/malformed lines) into this store.
    void parse_ini(const char* data, size_t size);

    // Serializes all entries as `key=value\n` into `out`. Returns bytes written (excluding NUL).
    size_t format_ini(char* out, size_t out_size) const;

    // Copies all entries from `overrides` on top of this store (keys in `overrides` win).
    void merge_from(const SettingsStore& overrides);

private:
    Entry* find(const char* key);
    const Entry* find(const char* key) const;

    Entry entries_[kMaxEntries]{};
    int count_ = 0;
};

// ------------------------------------------------------------------------------------------------
// MenuHost: native ConfigMenuItem vtable cloning, root/group/subgroup registries, and menu_closed
// lifecycle tracking. Platform code only supplies call_game and the 6 vtable slot addresses.
// ------------------------------------------------------------------------------------------------
constexpr size_t kMaxUiItemVtableBytes = 0x248;

struct PluginMenuItem {
    uint8_t native[0xA0];       // engine ConfigMenuItem header
    PluginMenuItem* owner;      // 0xA0: always self
    const FefwMenuEntry* entry; // 0xA8
    uint64_t table[kMaxUiItemVtableBytes / 8];
    FefwMenuStepFn step;        // Left / Right, set by menu_set_step
};
static_assert(offsetof(PluginMenuItem, owner) == 0xA0, "native ConfigMenuItem prefix");
static_assert(sizeof(PluginMenuItem) <= 0x300, "menu items are allocated as 0x300 bytes");

struct MenuPlatform {
    uintptr_t main_base = 0;
    uint64_t (*call_game)(uintptr_t addr, const uint64_t* args, int nargs) = nullptr;
    uint64_t slot_text = 0;
    uint64_t slot_confirm = 0;
    uint64_t slot_left = 0;
    uint64_t slot_right = 0;
    uint64_t slot_dtor[2] = {0, 0};
    void (*lock_fn)() = nullptr;    // optional
    void (*unlock_fn)() = nullptr;  // optional
};

class MenuHost {
public:
    static constexpr int kMaxCallbacks = 32;
    static constexpr int kMaxMenuGroups = 8;
    static constexpr int kMaxMenuSubgroups = 16;
    static constexpr int kKeyLen = 64;

    bool init(const FefwMenuConfig& cfg, const MenuPlatform& plat);
    void disable();
    bool is_ready() const { return ready_; }

    int register_system_menu_provider(FefwMenuProviderFn fn, void* user);
    int register_menu_group_provider(const char* group_name, FefwMenuProviderFn fn, void* user);
    int register_menu_group_provider_ordered(const char* group_name, int order, FefwMenuProviderFn fn, void* user);
    int register_menu_subgroup_provider(const char* group_name, const char* sub_id, int sub_order,
                                        const char* (*title_fn)(void* user, void* item), void* title_user,
                                        int item_order, const char* item_key, FefwMenuProviderFn fn, void* user);
    int register_menu_closed_callback(FefwMenuClosedFn fn, void* user);

    void* add_entry(void* menu, const FefwMenuEntry* entry);
    void* open_submenu(void* item);
    void refresh(void* item);
    static int set_step(void* item, FefwMenuStepFn fn);

    // Invoked from the game's system_menu mid-hook with the menu container pointer.
    void on_system_menu(void* menu);

    // Invoked once per frame tick; runs registered menu_closed callbacks once live_rows reaches 0.
    void flush_menu_closed();

    // Vtable dispatch helpers called by the platform's native functions or make_function thunks.
    static const char* handle_text(PluginMenuItem* item);
    static int handle_confirm(PluginMenuItem* item);
    static int handle_step(PluginMenuItem* item, int dir);
    void handle_dtor(PluginMenuItem* item, int which);

    int live_rows() const;
    bool has_menu_closed_callbacks() const;

private:
    struct MenuProvider {
        FefwMenuProviderFn fn;
        void* user;
    };
    struct GroupProvider {
        FefwMenuProviderFn fn;
        void* user;
        int order;
        char item_key[kKeyLen];
    };
    struct MenuSubgroup {
        MenuHost* host;
        char id[kKeyLen];
        int group_idx;
        int parent_sub_idx;
        int order;
        bool has_order;
        const char* (*title_fn)(void* user, void* item);
        void* title_user;
        GroupProvider providers[kMaxCallbacks];
        volatile int count;
        FefwMenuEntry entry;
    };
    struct MenuGroup {
        MenuHost* host;
        char name[kKeyLen];
        GroupProvider providers[kMaxCallbacks];
        volatile int count;
        FefwMenuEntry entry;
    };
    struct MenuClosedCallback {
        FefwMenuClosedFn fn;
        void* user;
    };

    void lock() const;
    void unlock() const;

    static const char* subgroup_menu_text(void* user, void* item);
    static int subgroup_menu_confirm(void* user, void* item);
    static void provide_subgroup_entry(void* user, void* menu);
    static const char* group_menu_text(void* user, void* item);
    static int group_menu_confirm(void* user, void* item);

    FefwMenuConfig cfg_{};
    MenuPlatform plat_{};
    bool ready_ = false;

    MenuProvider menu_providers_[kMaxCallbacks]{};
    volatile int menu_provider_count_ = 0;

    MenuGroup menu_groups_[kMaxMenuGroups]{};
    volatile int menu_group_count_ = 0;

    MenuSubgroup menu_subgroups_[kMaxMenuSubgroups]{};
    volatile int menu_subgroup_count_ = 0;

    MenuClosedCallback menu_closed_[kMaxCallbacks]{};
    volatile int menu_closed_count_ = 0;

    volatile int live_rows_ = 0;
    volatile int menu_closed_pending_ = 0;
    uint64_t orig_dtor_[2] = {0, 0};
};

// ------------------------------------------------------------------------------------------------
// ServiceRegistry: inter-plugin named pointer registry shared by Switch & PC hosts.
// ------------------------------------------------------------------------------------------------
class ServiceRegistry {
public:
    static constexpr int kMaxServices = 32;
    static constexpr int kNameLen = 64;

    void set_lock(void (*lock_fn)(), void (*unlock_fn)()) {
        lock_fn_ = lock_fn;
        unlock_fn_ = unlock_fn;
    }

    int register_service(const char* name, void* ptr);
    void* get_service(const char* name) const;
    int count() const;

private:
    struct Entry {
        char name[kNameLen];
        void* ptr;
    };

    Entry entries_[kMaxServices]{};
    volatile int count_ = 0;
    void (*lock_fn_)() = nullptr;
    void (*unlock_fn_)() = nullptr;
};

}  // namespace fefw_foundation
#endif
