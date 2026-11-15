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

#define FEFW_PLUGIN_API_VERSION 4u

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
typedef void (*FefwMenuClosedFn)(void* user);               // the menu holding plugin rows has closed

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

    // Mid-function hooks placed by the bootstrap. Hook sites and their names come from the game profile
    // (docs/GAME_PROFILE.md); `name` selects every site with that name. `ctx` holds x0..x30 at the hook site and
    // is written back to the registers afterwards. Returns 0 if the profile has no hook of that name (or the
    // callback table is full). Check `size` before using fields added after this point.
    int (*register_mid_hook)(const char* name, FefwMidHookFn fn, void* user);

    // The game's current text language as its /Msg/<lang>/ folder code ("USen", "JPja", "CNzh", "TWzh", "KRko",
    // "EUfr", ...), taken from the last message file the game opened. "" until the game has loaded any message
    // file. Follows in-game language changes. Check `size` before calling.
    const char* (*get_language)(void);

    // (version 3) Calls the game function at absolute address `addr` with x0..x(nargs-1) = args[0..nargs-1] (integer and
    // pointer arguments only, nargs <= 8) and returns x0. On the Switch this is a plain call; hosts where game code does not
    // run natively (a PC recompilation) route it through their own call mechanism, so a plugin that calls game functions
    // only through call_game runs on both. Call it from a callback the runtime made (frame, menu, hook), not from a thread
    // of the plugin's own. Check `size` before calling; without it, call the address directly.
    uint64_t (*call_game)(uintptr_t addr, const uint64_t* args, int nargs);

    // (version 4) Called on the game thread (from the frame tick) once the game has destroyed every plugin menu row,
    // i.e. the menu that held them, root rows included, is closed. Use it to start something chosen in the menu only
    // after the menu is gone, so the two never take input together. Check `size` before calling.
    int (*register_menu_closed_callback)(FefwMenuClosedFn fn, void* user);
} FefwApi;

typedef int (*FefwPluginMainFn)(const FefwApi* api);

#ifdef __cplusplus
}
#endif
