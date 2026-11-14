// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — In-Memory MSBT (.msbt / .msbt.zst) Label & Text Patcher
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fefw_foundation {

// Zstandard helpers
bool zstd_decompress(const uint8_t* src, size_t src_len, std::vector<uint8_t>& out_data, std::string* err = nullptr);
bool zstd_compress(const uint8_t* src, size_t src_len, std::vector<uint8_t>& out_data, int level = 3, std::string* err = nullptr);

// Attribute (ATR1) record for a NEW label. FEFW records are 74 bytes whose string fields are u32 offsets into
// the section's string pool (e.g. byte 12 = speaker name, 24 = expression, 28 = motion script in talk files).
struct MsbtAtrSpec {
    std::string template_label;                  // copy this label's record (empty: first record, strings blanked)
    std::map<int, std::string> string_overrides; // record byte offset -> UTF-8 string
};

// Patches an uncompressed or Zstd-compressed MSBT binary with new/updated UTF-8 label->text entries.
// Automatically converts UTF-8 strings to UTF-16LE when the MSBT header specifies UTF-16LE encoding (0x0C == 1).
// When the file has an ATR1 section, every NEW label also gets an attribute record (from `atr_specs` if given,
// otherwise a blank one) so the attribute table stays parallel to the text table.
bool patch_msbt_archive(const std::vector<uint8_t>& input_bytes,
                        const std::map<std::string, std::string>& label_to_utf8_text,
                        std::vector<uint8_t>& output_bytes,
                        std::string* err = nullptr,
                        const std::map<std::string, MsbtAtrSpec>* atr_specs = nullptr);

// Inspects an MSBT (.msbt or .msbt.zst) and extracts all label->UTF-8 text pairs for verification.
bool read_msbt_messages(const std::vector<uint8_t>& input_bytes,
                        std::map<std::string, std::string>& out_messages,
                        std::string* err = nullptr);

}  // namespace fefw_foundation
