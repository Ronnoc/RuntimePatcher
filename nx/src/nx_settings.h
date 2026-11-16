// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — persistent plugin settings (see nx_settings.cpp).
#pragma once

#include <stddef.h>

#include "fefw_foundation/plugin_api.h"

namespace fefw_nx {
int settings_get(const char* key, char* out, size_t out_size);  // FefwApi::settings_get
int settings_set(const char* key, const char* value);           // FefwApi::settings_set
void settings_flush();  // frame tick: writes values set before the save mount existed
}  // namespace fefw_nx
