// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — minimal Horizon runtime glue for the subsdk9 module.
#include "nx_runtime.h"

#include <stdarg.h>

extern "C" {
int vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);  // imported from sdk (nnmusl)
size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
}

namespace fefw_nx {

uint32_t svc_query_memory(MemoryInfo* out, uintptr_t addr) {
    register uint64_t x0 asm("x0") = reinterpret_cast<uint64_t>(out);
    register uint64_t x1 asm("x1") = 0;
    register uint64_t x2 asm("x2") = addr;
    asm volatile("svc 0x6" : "+r"(x0), "+r"(x1), "+r"(x2) : : "memory", "x3", "x4", "x5", "x6", "x7");
    return static_cast<uint32_t>(x0);
}

void svc_output_debug_string(const char* str, size_t len) {
    register uint64_t x0 asm("x0") = reinterpret_cast<uint64_t>(str);
    register uint64_t x1 asm("x1") = len;
    asm volatile("svc 0x27" : "+r"(x0), "+r"(x1) : : "memory");
}

void vlog(const char* fmt, va_list ap) {
    char buf[512];
    static const char kPrefix[] = "[FEFW] ";
    size_t p = sizeof(kPrefix) - 1;
    for (size_t i = 0; i < p; ++i) buf[i] = kPrefix[i];
    vsnprintf(buf + p, sizeof(buf) - p, fmt, ap);
    svc_output_debug_string(buf, strlen(buf));
}

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

bool is_readable(uintptr_t addr, size_t size) {
    if (!addr || addr + size < addr) return false;
    uintptr_t end = addr + (size ? size : 1);
    while (addr < end) {
        MemoryInfo mi{};
        if (svc_query_memory(&mi, addr) != 0 || (mi.perm & 1) == 0 || mi.type == 0) return false;
        uintptr_t next = mi.addr + mi.size;
        if (next <= addr) return false;
        addr = next;
    }
    return true;
}

uintptr_t find_module_base(uintptr_t addr_in_text) {
    MemoryInfo mi{};
    if (svc_query_memory(&mi, addr_in_text) != 0) return 0;
    return static_cast<uintptr_t>(mi.addr);  // .text is mapped at offset 0 of the module
}

namespace {

constexpr int64_t DT_NULL = 0, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_GNU_HASH = 0x6ffffef5;

struct Elf64Sym {
    uint32_t st_name;
    uint8_t st_info;
    uint8_t st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
};

uint32_t sysv_hash(const char* s) {
    uint32_t h = 0;
    for (; *s; ++s) {
        h = (h << 4) + static_cast<uint8_t>(*s);
        uint32_t g = h & 0xF0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

uint32_t gnu_hash(const char* s) {
    uint32_t h = 5381;
    for (; *s; ++s) h = h * 33 + static_cast<uint8_t>(*s);
    return h;
}

}  // namespace

uintptr_t lookup_module_symbol(uintptr_t base, const char* name) {
    if (!base) return 0;
    uint32_t mod0_off = *reinterpret_cast<const uint32_t*>(base + 4);
    uintptr_t mod0 = base + mod0_off;
    if (*reinterpret_cast<const uint32_t*>(mod0) != 0x304F444Du /* "MOD0" */) return 0;
    auto* dyn = reinterpret_cast<const int64_t*>(mod0 + *reinterpret_cast<const int32_t*>(mod0 + 4));

    auto fix = [base](uint64_t v) -> uintptr_t { return v < base ? base + v : v; };
    uintptr_t hash = 0, gnuhash = 0, strtab = 0, symtab = 0;
    for (; dyn[0] != DT_NULL; dyn += 2) {
        switch (dyn[0]) {
            case DT_HASH: hash = fix(dyn[1]); break;
            case DT_GNU_HASH: gnuhash = fix(dyn[1]); break;
            case DT_STRTAB: strtab = fix(dyn[1]); break;
            case DT_SYMTAB: symtab = fix(dyn[1]); break;
            default: break;
        }
    }
    if (!strtab || !symtab) return 0;
    auto* syms = reinterpret_cast<const Elf64Sym*>(symtab);
    auto* strs = reinterpret_cast<const char*>(strtab);

    auto match = [&](uint32_t idx) -> uintptr_t {
        const Elf64Sym& s = syms[idx];
        if (s.st_shndx != 0 && s.st_value != 0 && strcmp(strs + s.st_name, name) == 0) return base + s.st_value;
        return 0;
    };

    if (hash) {
        auto* h = reinterpret_cast<const uint32_t*>(hash);
        uint32_t nbucket = h[0];
        const uint32_t* bucket = h + 2;
        const uint32_t* chain = bucket + nbucket;
        for (uint32_t i = bucket[sysv_hash(name) % nbucket]; i != 0; i = chain[i]) {
            if (uintptr_t r = match(i)) return r;
        }
        return 0;
    }
    if (gnuhash) {
        auto* h = reinterpret_cast<const uint32_t*>(gnuhash);
        uint32_t nbuckets = h[0], symoffset = h[1], bloom_size = h[2];
        auto* buckets = reinterpret_cast<const uint32_t*>(gnuhash + 16 + bloom_size * 8);
        const uint32_t* chain = buckets + nbuckets;
        uint32_t hv = gnu_hash(name);
        uint32_t i = buckets[hv % nbuckets];
        if (i < symoffset) return 0;
        for (;; ++i) {
            uint32_t ch = chain[i - symoffset];
            if ((ch | 1) == (hv | 1)) {
                if (uintptr_t r = match(i)) return r;
            }
            if (ch & 1) break;
        }
    }
    return 0;
}

uintptr_t lookup_symbol_in_other_modules(uintptr_t skip_base, const char* name) {
    // NSO .text segments are mapped R-X and start with the module header (u32 MOD0 offset at +4).
    // The memory *state* of static code differs between firmware/emulator versions (Eden's Switch 2
    // mode does not report plain 0x03), so identify modules by permission + a valid MOD0 instead.
    constexpr uint32_t kPermRX = 5;
    uintptr_t addr = 0;
    for (int guard = 0; guard < 0x10000; ++guard) {
        MemoryInfo mi{};
        if (svc_query_memory(&mi, addr) != 0) break;
        if ((mi.perm & 7) == kPermRX && mi.addr != skip_base && mi.size >= 0x1000) {
            uint32_t mod0_off = *reinterpret_cast<const uint32_t*>(mi.addr + 4);
            if (mod0_off >= 8 && (mod0_off & 3) == 0 && mod0_off < (1u << 30) &&
                *reinterpret_cast<const uint32_t*>(mi.addr + mod0_off) == 0x304F444Du) {
                if (uintptr_t r = lookup_module_symbol(mi.addr, name)) return r;
            }
        }
        uintptr_t next = mi.addr + mi.size;
        if (next <= addr) break;
        addr = next;
    }
    return 0;
}

void log_code_regions() {
    uintptr_t addr = 0;
    for (int guard = 0; guard < 0x10000; ++guard) {
        MemoryInfo mi{};
        if (svc_query_memory(&mi, addr) != 0) break;
        if ((mi.perm & 7) == 5) {
            uint32_t mod0_off = *reinterpret_cast<const uint32_t*>(mi.addr + 4);
            bool has_mod0 = mod0_off >= 8 && (mod0_off & 3) == 0 && mod0_off < (1u << 30) &&
                            *reinterpret_cast<const uint32_t*>(mi.addr + mod0_off) == 0x304F444Du;
            const uint32_t* w = reinterpret_cast<const uint32_t*>(mi.addr);
            MemoryInfo m2{};
            uint32_t q = svc_query_memory(&m2, mi.addr + mod0_off);
            log("  RX region %p size 0x%llx state 0x%x perm %u w0=%08x w1=%08x mod0 %s | @mod0: rc=%x base=%p size=0x%llx perm=%u magic=%08x",
                reinterpret_cast<void*>(mi.addr), static_cast<unsigned long long>(mi.size), mi.type, mi.perm, w[0], w[1],
                has_mod0 ? "yes" : "no", q, reinterpret_cast<void*>(m2.addr), static_cast<unsigned long long>(m2.size),
                m2.perm, q == 0 && (m2.perm & 1) ? *reinterpret_cast<const uint32_t*>(mi.addr + mod0_off) : 0u);
        }
        uintptr_t next = mi.addr + mi.size;
        if (next <= addr) break;
        addr = next;
    }
}

void sleep_ns(int64_t ns) {
    register int64_t x0 asm("x0") = ns;
    asm volatile("svc 0xB" : "+r"(x0) : : "memory", "x1", "x2", "x3", "x4", "x5", "x6", "x7");
}

}  // namespace fefw_nx
