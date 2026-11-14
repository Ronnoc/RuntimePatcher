// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — minimal Horizon runtime glue for the subsdk9 module.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define FEFW_EXPORT __attribute__((visibility("default")))

// 1 when building the foundation as an NRO (`--loader nro`), 0 for the static subsdk9 module.
#ifndef FEFW_NX_NRO
#define FEFW_NX_NRO 0
#endif

namespace fefw_nx {

struct MemoryInfo {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
    uint32_t attr;
    uint32_t perm;
    uint32_t ipc_refcount;
    uint32_t device_refcount;
    uint32_t padding;
    // Slack: Switch 2 kernels / Eden NX2 mode may write a larger MemoryInfo than 0x28 bytes.
    uint8_t reserved[0x40];
};

uint32_t svc_query_memory(MemoryInfo* out, uintptr_t addr);
void svc_output_debug_string(const char* str, size_t len);

// printf-style logging through svcOutputDebugString (visible in Eden log as Debug_Emulated).
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void vlog(const char* fmt, __builtin_va_list ap);

// True if [addr, addr+size) is mapped and readable (svcQueryMemory).
bool is_readable(uintptr_t addr, size_t size);

// Finds the module (NSO) containing `addr_in_text` and resolves an exported dynamic symbol by name.
// Returns 0 if not found.
uintptr_t find_module_base(uintptr_t addr_in_text);
uintptr_t lookup_module_symbol(uintptr_t module_base, const char* name);

// Resolves `name` in every loaded module except `skip_base` (our own module), in address order.
// Returns 0 if no other module defines it.
uintptr_t lookup_symbol_in_other_modules(uintptr_t skip_base, const char* name);

// Diagnostics: logs every R-X region and whether it carries a MOD0 header.
void log_code_regions();

void sleep_ns(int64_t ns);

// Resolves an sdk export: dynamic symbol lookup, then the build-time offset table (fefw_sdk_offsets.h).
uintptr_t resolve_sdk_symbol(const char* name);

// Plugin host (nx_game_hooks.cpp): loads <mount>:/fefw/plugins/<name>.nro (+ .nrr) through nn::ro and
// calls each plugin's fefw_plugin_main. Called once, after the class overlay is built.
using PluginReadFn = bool (*)(const char* path, void** out_page_aligned, size_t* out_size);
using PluginListFn = bool (*)(const char* dir, void (*on_file)(void* ctx, const char* name), void* ctx);
void load_plugins(const char* mount, PluginReadFn read_file, PluginListFn list_dir);
// Records the game's text language from a RomFS path like "/Msg/USen/Job.msbt.zst" (no-op for other paths).
void note_game_file(const char* vpath);

}  // namespace fefw_nx
