// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — plugin ABI (C, stable).
//
// A plugin is a separate NRO shipped by its own Eden add-on:
//     <AddOn>/romfs/fefw/plugins/<name>.nro + <name>.nrr
// After the class overlay is built, the runtime loads every plugin through nn::ro and calls its exported
//     extern "C" int fefw_plugin_main(const FefwApi* api);
// The plugin keeps `api` for its whole lifetime. Plugins never import symbols from each other or from the
// runtime: everything they need from the runtime goes through this table.
//
// Game-side services (menu, frame tick, input injection) need the NRO loader's bootstrap hooks
// (`--loader nro`, the default). With the static subsdk9 loader they are registered but never called.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FEFW_PLUGIN_API_VERSION 1u

// nn::hid::NpadButton bits (subset).
#define FEFW_BUTTON_A (1ull << 0)
#define FEFW_BUTTON_B (1ull << 1)
#define FEFW_BUTTON_PLUS (1ull << 10)
#define FEFW_BUTTON_MINUS (1ull << 11)

// Return values of FefwMenuEntry::confirm (the game's ConfigMenuItem confirm protocol).
#define FEFW_MENU_IGNORED 1   // nothing happened
#define FEFW_MENU_OPENED 10   // a submenu was opened with FefwApi::menu_open_submenu
#define FEFW_MENU_HANDLED 13  // handled in place (menu stays open)

// One row of a game config menu, owned by the plugin (must outlive the menu; use static storage).
typedef struct FefwMenuEntry {
    // Row text (UTF-8). Called by the game every time the row is drawn or refreshed.
    const char* (*text)(void* user, void* item);
    // A pressed on the row. May call menu_open_submenu(item) and fill the returned menu.
    int (*confirm)(void* user, void* item);
    void* user;
} FefwMenuEntry;

typedef void (*FefwMenuProviderFn)(void* user, void* menu);  // append rows to `menu`
typedef void (*FefwFrameFn)(void* user, uint64_t frame);     // once per game frame, game thread
typedef void (*FefwMidHookFn)(void* user, uint64_t* ctx);    // ctx[0..30] = x0..x30

// Mid-function hook ids (sites are defined by the game profile, see tools/game_profile.py).
// Hub breakable hit gate: ctx[19] = the gate's result (1 = accept the hit).
#define FEFW_MID_HUB_HIT_GATE 1u
#define FEFW_MID_TALK_WINDOW 2u  // hub talk window created: x0 = const std::string* label
#define FEFW_MID_POST_TALK 3u    // after a hub talk event: x0 = character handle; script context live
// Battle EXP: ctx[0] = EXP this unit gains (write to change it), ctx[19] = game Unit*.
#define FEFW_MID_BATTLE_EXP 4u
// Chest / loot access, just before the camera focus, which runs only if x24 (player) != 0.
// ctx[24] = 0 skips the camera focus request and pickup-focus registration.
#define FEFW_MID_PICKUP_CAMERA 5u
// Loot access, right before it runs the hub script named by x1 (the item popup). x19 = loot controller.
#define FEFW_MID_PICKUP_SCRIPT 6u
// Hub loot item gain: x1 = the std::vector<UnitItem>* to be gained and shown in the item window. Replacing x1
// changes the whole list; any mod that hooks the call itself then sees the replaced list.
#define FEFW_MID_LOOT_GAIN_CALL 7u

typedef struct FefwApi {
    uint32_t version;  // FEFW_PLUGIN_API_VERSION
    uint32_t size;     // sizeof(FefwApi)

    // Base address of the game's main module (.text start). Game offsets in plugins are main-relative.
    uintptr_t main_base;

    void (*log)(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Called while the game builds its System settings menu: append root rows there.
    int (*register_system_menu_provider)(FefwMenuProviderFn fn, void* user);
    // Called once per frame from the game's input poll (nn::hid::GetNpadState).
    int (*register_frame_callback)(FefwFrameFn fn, void* user);
    // ORs `buttons` into every npad state the game reads for the next `frames` frames.
    void (*inject_buttons)(uint64_t buttons, uint32_t frames);

    // Appends a row to a game menu container. Returns the native item (or NULL).
    void* (*menu_add_entry)(void* menu, const FefwMenuEntry* entry);
    // From inside FefwMenuEntry::confirm: opens a child menu below `item` and returns its container.
    void* (*menu_open_submenu)(void* item);
    // Redraws `item` (text() is called again).
    void (*menu_refresh)(void* item);

    // Readable-memory probe (svcQueryMemory): 1 if [addr, addr+size) is mapped readable.
    int (*is_readable)(uintptr_t addr, size_t size);

    // Mid-function hooks placed by the bootstrap (FEFW_MID_*). `ctx` holds x0..x30 at the hook site and is
    // written back to the registers afterwards. Check `size` before using fields added after this point.
    int (*register_mid_hook)(uint32_t hook_id, FefwMidHookFn fn, void* user);

    // The game's current text language as its /Msg/<lang>/ folder code ("USen", "JPja", "CNzh", "TWzh", "KRko",
    // "EUfr", ...), taken from the last message file the game opened. "" until the game has loaded any message
    // file. Follows in-game language changes. Check `size` before calling.
    const char* (*get_language)(void);
} FefwApi;

typedef int (*FefwPluginMainFn)(const FefwApi* api);

#ifdef __cplusplus
}
#endif
