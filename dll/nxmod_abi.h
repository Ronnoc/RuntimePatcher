// SPDX-License-Identifier: MIT
// Minimal declaration of the code-mod interface of PC recompilation runtimes that load mods as DLLs
// (`mods/<name>/exefs/*.dll`, exports `nxmod_early` / `nxmod_init`, interface version >= 3).
//
// Written for interoperability: only the table layout these mods rely on is declared. The layout is: two uint32
// fields, a string pointer, a uint64, then one pointer per entry in the order below. The guest CPU context is opaque
// except for its first member, the general registers: x[0..30], x[31] = SP (NxModRegs).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NxCtx NxCtx;  // the runtime's guest CPU context
#define NxModRegs(c) ((uint64_t*)(c))  // its x0..x30 (+ SP), readable and writable

typedef void (*NxModInlineFn)(NxCtx* c, uint64_t addr, void* user);
typedef int (*NxModSiteFn)(NxCtx* c, uint64_t addr, void* user);  // nonzero = handled (skips the rest of the function)
typedef void (*NxModFrameFn)(NxCtx* c, void* user);
typedef void (*NxModFunctionFn)(NxCtx* c, void* user);  // arguments in x0..x7, the result goes in x0

typedef struct NxModFile {
    const char* path;
    const uint8_t* data;
    uint64_t size;
    void (*replace)(const struct NxModFile* file, const void* data, uint64_t size);
} NxModFile;
typedef void (*NxModFileFn)(const NxModFile* file, void* user);

typedef struct NxModApi {
    uint32_t version;
    uint32_t size;
    const char* game_id;
    uint64_t main_base;
    void (*log)(const struct NxModApi* api, int level, const char* text);  // 0 error, 1 warning, 2 info, 3 debug
    // hooks at the sites the game was lifted with (absolute guest addresses); 0 = failure
    uint32_t (*hook_inline)(uint64_t addr, NxModInlineFn fn, void* user);
    uint32_t (*hook_mid)(uint64_t addr, NxModSiteFn fn, void* user);
    uint32_t (*hook_entry)(uint64_t addr, NxModSiteFn fn, void* user);
    int (*hook_remove)(uint32_t id);
    // calls a lifted guest function with x0..x7 = args; only from a hook, frame or function callback, with its context
    uint64_t (*call_guest)(NxCtx* c, uint64_t addr, const uint64_t* args, int nargs);
    int (*is_mapped)(uint64_t addr, uint64_t size);
    void (*on_frame)(NxModFrameFn fn, void* user);  // once per presented frame, main thread
    int (*is_function)(uint64_t addr);              // a lifted function entry (a lifted midhook site is one too)
    // version 2
    uint64_t (*make_function)(NxModFunctionFn fn, void* user);  // a guest address the game can call
    int (*make_writable)(uint64_t addr, uint64_t size);
    // version 3
    int (*edit_file)(const char* path, NxModFileFn fn, void* user);
    uint32_t (*mod_count)(void);
    const char* (*mod_folder)(uint32_t index);
    int64_t (*read_file)(const char* path, void* buf, uint64_t cap);
    // (later entries are not used)
} NxModApi;

#define NXMOD_MIN_VERSION 3u

#ifdef __cplusplus
}
#endif
