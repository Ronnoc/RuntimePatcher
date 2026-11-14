// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — In-Memory SARC.zst + BNTX Texture Cloner & Patricia Trie Builder
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fefw_foundation {

struct BntxTexturePatch {
    std::string new_name;
    std::string clone_from_name;
    std::vector<uint8_t> custom_swizzled_data;  // empty = keep cloned texture's pixel data
};

// Swizzles linear 32-bit RGBA pixels (width x height x 4 bytes) into Switch 2 Morton tiled layout
std::vector<uint8_t> swizzle_rgba_switch2(const uint8_t* linear_rgba, int width, int height, int block_height_log2 = 2);

// Swizzles linear BC7 blocks ((width/4) x (height/4) x 16 bytes) into Switch 2 Morton tiled layout
std::vector<uint8_t> swizzle_bc7_switch2(const uint8_t* linear_bc7, int width, int height, int block_height_log2 = 0);

// Patches a SARC.zst (such as Icon_Job.arc.zst or Icon_Skill.arc.zst) in-memory:
// 1. Decompresses .arc.zst
// 2. Extracts timg/__Combined.bntx
// 3. Clones/injects all `patches` into the BNTX container
// 4. Rebuilds the Nintendo Patricia trie (ResDict) and relocation table (_RLT)
// 5. Wraps in SARC and compresses back to .arc.zst
bool patch_sarc_bntx_archive(const std::vector<uint8_t>& input_arc_zst,
                             const std::vector<BntxTexturePatch>& patches,
                             std::vector<uint8_t>& output_arc_zst,
                             std::string* err = nullptr);

// Inspects a SARC.zst containing a BNTX and returns all texture names present in its StringTable / ResDict.
bool list_sarc_bntx_textures(const std::vector<uint8_t>& input_arc_zst,
                             std::vector<std::string>& out_texture_names,
                             std::string* err = nullptr);

}  // namespace fefw_foundation
