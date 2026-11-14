// SPDX-License-Identifier: MIT
// FEFW NRO probe: proves nn::ro can load + bind an NRO in this game/emulator. No imports at all.
#include <stddef.h>
#include <stdint.h>

static void debug_string(const char* s, size_t n) {
    register uint64_t x0 asm("x0") = reinterpret_cast<uint64_t>(s);
    register uint64_t x1 asm("x1") = n;
    asm volatile("svc 0x27" : "+r"(x0), "+r"(x1) : : "memory");
}

static int g_counter = 41;  // .data: checks relocation + writable data mapping

extern "C" __attribute__((visibility("default"))) uint32_t fefw_probe_hello() {
    static const char kMsg[] = "[FEFW-NRO] hello from dynamically loaded NRO";
    debug_string(kMsg, sizeof(kMsg) - 1);
    return 0xFEF00000u | static_cast<uint32_t>(++g_counter);  // expect 0xFEF0002A
}
