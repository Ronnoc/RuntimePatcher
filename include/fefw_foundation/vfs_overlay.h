// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — Universal Virtual RomFS Table Overlay Engine
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "fefw_foundation/bntx_patcher.h"
#include "fefw_foundation/game_table.h"
#include "fefw_foundation/msbt_patcher.h"

namespace fefw_foundation {

// Callback provided by the host VFS (e.g. a PC port's RomFS layer) to read an existing
// RomFS file by its virtual path (e.g. "/GameData/Job.bin", "/Msg/CNzh/Job.msbt.zst").
// Note: If a third-party mod replaced "/GameData/Job.bin" in an overlay folder, `read_base_file`
// returns that third-party modded Job.bin! Our foundation then merges all modular class/skill patches
// directly on top of it so both coexist 100% out-of-the-box.
using ReadVirtualFileFn = std::function<bool(const std::string& virtual_path, std::vector<uint8_t>& out_bytes)>;

struct VfsOverlayResult {
    // Map from virtual RomFS path (e.g. "/GameData/Job.bin") to synthesized in-memory file bytes
    std::map<std::string, std::vector<uint8_t>> synthesized_files;
    // Names of modular mods discovered and applied
    std::vector<std::string> applied_mods;
};

// Result of the cheap first pass: which mods were found and which RomFS files they patch. The patched bytes are
// built later, one file at a time, by synthesize_vfs_file() (all mod resources are read while planning, so
// building only needs the base files).
struct VfsOverlayPlan {
    std::vector<std::string> applied_mods;
    std::set<std::string> target_paths;  // virtual paths synthesize_vfs_file() can build
    struct Impl;
    Impl* impl = nullptr;  // owned (no shared_ptr: the Switch runtime has no libc++ shared-count support)
    bool targets(const std::string& vpath) const;

    VfsOverlayPlan() = default;
    ~VfsOverlayPlan();
    VfsOverlayPlan(const VfsOverlayPlan&) = delete;
    VfsOverlayPlan& operator=(const VfsOverlayPlan&) = delete;
};

// Platform-neutral access to the storage that holds the mod packages (overlay dirs).
// Paths are UTF-8 strings joined with '/'. On PC this is backed by std::filesystem; inside the Switch
// subsdk module it is backed by nn::fs on the game's RomFS mount (e.g. "Contents:/GameData/Classes").
struct HostDirEntry {
    std::string name;
    bool is_dir = false;
};

struct HostFs {
    // Reads a whole file. Returns false if it does not exist / cannot be read.
    std::function<bool(const std::string& path, std::vector<uint8_t>& out_bytes)> read_file;
    // Lists the direct children of a directory. Returns false if `dir` is not a directory.
    std::function<bool(const std::string& dir, std::vector<HostDirEntry>& out_entries)> list_dir;
};

#ifndef FEFW_FOUNDATION_NO_STD_FILESYSTEM
// HostFs implementation backed by std::filesystem (PC builds).
HostFs make_std_host_fs();

// Scans `overlay_dirs` (or discovered `/GameData/Classes/*.delta` inside the virtual tree) for modular
// FEFW RuntimePatcher mods (`manifest.json` or `.delta`), merges their patches onto the base RomFS files
// via `read_base_file`, and outputs the synthesized in-memory files into `out_result`.
bool apply_vfs_overlays(const std::vector<std::string>& overlay_dirs,
                        const ReadVirtualFileFn& read_base_file,
                        VfsOverlayResult& out_result,
                        std::string* err = nullptr);
#endif

// Same as above, but mod packages are read through `host_fs` instead of std::filesystem.
bool apply_vfs_overlays(const std::vector<std::string>& overlay_dirs,
                        const ReadVirtualFileFn& read_base_file,
                        const HostFs& host_fs,
                        VfsOverlayResult& out_result,
                        std::string* err = nullptr);

// First pass: discover mods under `overlay_dirs` (through `host_fs`) and collect their patches.
bool plan_vfs_overlays(const std::vector<std::string>& overlay_dirs,
                       const HostFs& host_fs,
                       VfsOverlayPlan& plan,
                       std::string* err = nullptr);

// Builds the patched bytes of one planned file. Returns false if `vpath` is not a target or its base is missing.
bool synthesize_vfs_file(const VfsOverlayPlan& plan,
                         const std::string& vpath,
                         const ReadVirtualFileFn& read_base_file,
                         std::vector<uint8_t>& out,
                         std::string* err = nullptr);

}  // namespace fefw_foundation
