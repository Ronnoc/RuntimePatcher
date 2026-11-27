// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — PC recompilation plugin host attachment ABI (exported by fefw_runtime_patcher.dll).
//
// Each plugin DLL calls fefw_nxmod_host_attach() from its nxmod_init() to obtain the process-wide singleton FefwApi*
// hosted by fefw_runtime_patcher.dll. Game-specific addresses and hook sites are passed in from the plugin's generated
// hooks_gen.h so the public runtime DLL remains free of game addresses.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "fefw_foundation/plugin_api.h"
#include "fefw_foundation/plugin_host_core.h"
#include "nxmod_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FEFW_NXMOD_HOST_ABI_VERSION 1u

typedef struct FefwHostSite {
    const char* name;
    uint64_t va;
    int entry;  // 1 = hook_entry, 0 = hook_mid
} FefwHostSite;

typedef struct FefwHostGameConfig {
    uint32_t abi_version;  // FEFW_NXMOD_HOST_ABI_VERSION
    uint32_t size;         // sizeof(FefwHostGameConfig)
    uint64_t system_menu_reg;
    uint64_t menu_resume;
    uint32_t menu_resume_insn;
    uint32_t reserved;
    FefwMenuConfig menu;
} FefwHostGameConfig;

typedef const FefwApi* (*FefwNxmodHostAttachFn)(const NxModApi* nx_api, const char* mod_dir,
                                                const FefwHostGameConfig* game_cfg, const FefwHostSite* sites,
                                                int site_count);

#ifdef __cplusplus
}
#endif
