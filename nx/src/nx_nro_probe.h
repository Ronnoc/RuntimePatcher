// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

namespace fefw_nx {
using ReadWholeFn = bool (*)(const std::string& path, std::vector<uint8_t>& out);
// Loads <mount>:/fefw_probe/probe.nro (+ probe.nrr) through the game's nn::ro and calls fefw_probe_hello().
// Diagnostic only: every step is logged as "[FEFW] nro-probe: ...".
void run_nro_probe(const std::string& mount, ReadWholeFn read_whole);
}  // namespace fefw_nx
