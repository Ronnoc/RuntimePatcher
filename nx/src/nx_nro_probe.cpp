// SPDX-License-Identifier: MIT
// FEFW NRO probe — checks whether nn::ro (ldr:ro) can load an NRO in this game + emulator.
// If it can, the foundation can be loaded after boot instead of as a static subsdk module,
// leaving the static module layout (subsdk / sdk addresses) untouched.
#include "nx_nro_probe.h"

#include <stdlib.h>
#include <string.h>

#include "nx_runtime.h"

namespace fefw_nx {
namespace {

using Result = uint32_t;
using InitializeFn = Result (*)();
using GetBufferSizeFn = Result (*)(size_t*, const void*);
using RegisterModuleInfoFn = Result (*)(void*, const void*);
using LoadModuleFn = Result (*)(void*, const void*, void*, size_t, int);
using LookupModuleSymbolFn = Result (*)(uintptr_t*, const void*, const char*);

constexpr int kBindFlagNow = 1;

// Page-aligned, never freed: nn::ro maps the image as code and owns the bss buffer while loaded.
void* alloc_pages(size_t size) {
    size = (size + 0xFFF) & ~size_t(0xFFF);
    auto raw = reinterpret_cast<uintptr_t>(malloc(size + 0x1000));
    if (!raw) return nullptr;
    void* p = reinterpret_cast<void*>((raw + 0xFFF) & ~uintptr_t(0xFFF));
    memset(p, 0, size);
    return p;
}

template <typename T>
T sdk(const char* name) {
    T fn = reinterpret_cast<T>(resolve_sdk_symbol(name));
    if (!fn) log("nro-probe: cannot resolve %s", name);
    return fn;
}

}  // namespace

void run_nro_probe(const std::string& mount, ReadWholeFn read_whole) {
    log("nro-probe: start");
    auto ro_init = sdk<InitializeFn>("_ZN2nn2ro10InitializeEv");
    auto ro_bufsize = sdk<GetBufferSizeFn>("_ZN2nn2ro13GetBufferSizeEPmPKv");
    auto ro_register = sdk<RegisterModuleInfoFn>("_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv");
    auto ro_load = sdk<LoadModuleFn>("_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi");
    auto ro_lookup = sdk<LookupModuleSymbolFn>("_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc");
    if (!ro_init || !ro_bufsize || !ro_register || !ro_load || !ro_lookup) return;

    std::vector<uint8_t> nro, nrr;
    if (!read_whole(mount + ":/fefw_probe/probe.nro", nro) || !read_whole(mount + ":/fefw_probe/probe.nrr", nrr)) {
        log("nro-probe: cannot read probe.nro / probe.nrr");
        return;
    }
    log("nro-probe: read nro %zu bytes, nrr %zu bytes", nro.size(), nrr.size());

    Result r = ro_init();
    log("nro-probe: nn::ro::Initialize -> 0x%x", r);

    void* nrr_buf = alloc_pages(nrr.size());
    void* nro_buf = alloc_pages(nro.size());
    void* reg_info = alloc_pages(0x1000);   // nn::ro::RegistrationInfo (oversized on purpose)
    void* module = alloc_pages(0x1000);     // nn::ro::Module (oversized on purpose)
    if (!nrr_buf || !nro_buf || !reg_info || !module) {
        log("nro-probe: allocation failed");
        return;
    }
    memcpy(nrr_buf, nrr.data(), nrr.size());
    memcpy(nro_buf, nro.data(), nro.size());

    r = ro_register(reg_info, nrr_buf);
    log("nro-probe: nn::ro::RegisterModuleInfo -> 0x%x", r);
    if (r) return;

    size_t bss_size = 0;
    r = ro_bufsize(&bss_size, nro_buf);
    log("nro-probe: nn::ro::GetBufferSize -> 0x%x (bss buffer 0x%zx)", r, bss_size);
    if (r) return;
    void* bss = bss_size ? alloc_pages(bss_size) : nullptr;

    r = ro_load(module, nro_buf, bss, bss_size, kBindFlagNow);
    log("nro-probe: nn::ro::LoadModule -> 0x%x", r);
    if (r) return;

    uintptr_t fn = 0;
    r = ro_lookup(&fn, module, "fefw_probe_hello");
    log("nro-probe: nn::ro::LookupModuleSymbol(fefw_probe_hello) -> 0x%x @ %p", r, reinterpret_cast<void*>(fn));
    if (r || !fn) return;

    uint32_t v = reinterpret_cast<uint32_t (*)()>(fn)();
    log("nro-probe: fefw_probe_hello() returned 0x%x (%s)", v, v == 0xFEF0002Au ? "PASS" : "unexpected");
}

}  // namespace fefw_nx
