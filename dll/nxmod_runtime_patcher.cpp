// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher as a code mod (DLL) for PC recompilation runtimes.
//
// Install: <mods>/FEFW_RuntimePatcher/exefs/fefw_runtime_patcher.dll. At load (nxmod_early) the romfs/ folder of
// every installed mod is scanned for RuntimePatcher packages (manifest.json / GameData/Classes/*.delta) and an edit is
// registered for each file they patch. When the runtime builds its RomFS it hands us that file as it stands (the
// game's, or a RomFS mod's replacement) and we return the patched bytes: the same overlay the emulator build serves.
// Native plugins (NRO) are Switch code and are not loaded here.
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "fefw_foundation/vfs_overlay.h"
#include "nxmod_abi.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define NXMOD_EXPORT extern "C" __declspec(dllexport)
#else
#include <dirent.h>
#include <sys/stat.h>
#define NXMOD_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

const NxModApi* g_api = nullptr;
fefw_foundation::VfsOverlayPlan* g_plan = nullptr;
std::deque<std::string> g_targets;  // stable storage: the user pointer of an edit is its virtual path

void logf(int level, const char* fmt, ...) {
    if (!g_api || !g_api->log) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_api->log(g_api, level, buf);
}

bool host_read(const std::string& path, std::vector<uint8_t>& out) {
    const int64_t n = g_api->read_file(path.c_str(), nullptr, 0);
    if (n < 0) return false;
    out.resize(static_cast<size_t>(n));
    if (n == 0) return true;
    return g_api->read_file(path.c_str(), out.data(), out.size()) == n;
}

// The runtime offers no directory listing, so folders are listed with the OS.
#ifdef _WIN32
std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
bool host_list(const std::string& dir, std::vector<fefw_foundation::HostDirEntry>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((widen(dir) + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        out.push_back({narrow(fd.cFileName), (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return true;
}
#else
bool host_list(const std::string& dir, std::vector<fefw_foundation::HostDirEntry>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        struct stat st {};
        bool is_dir = stat((dir + "/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        out.push_back({name, is_dir});
    }
    closedir(d);
    return true;
}
#endif

void on_edit(const NxModFile* file, void* user) {
    const std::string& vpath = *static_cast<const std::string*>(user);
    auto read_base = [&](const std::string& vp, std::vector<uint8_t>& out) {
        if (vp != vpath || !file->data) return false;
        out.assign(file->data, file->data + file->size);
        return true;
    };
    std::vector<uint8_t> out;
    std::string err;
    if (fefw_foundation::synthesize_vfs_file(*g_plan, vpath, read_base, out, &err)) {
        file->replace(file, out.data(), out.size());
        logf(2, "patched %s (%llu -> %zu bytes)", vpath.c_str(), static_cast<unsigned long long>(file->size),
             out.size());
    } else {
        logf(1, "%s left as it is (%s)", vpath.c_str(), err.empty() ? "nothing to apply" : err.c_str());
    }
}

}  // namespace

NXMOD_EXPORT int nxmod_early(const NxModApi* api, const char* mod_dir) {
    (void)mod_dir;
    if (!api || api->version < NXMOD_MIN_VERSION || api->size < sizeof(NxModApi)) return 1;
    g_api = api;
    logf(2, "FEFW RuntimePatcher %s", FEFW_VERSION);

    std::vector<std::string> overlays;
    for (uint32_t i = 0, n = api->mod_count(); i < n; ++i) {
        if (const char* f = api->mod_folder(i)) overlays.push_back(std::string(f) + "/romfs");
    }
    fefw_foundation::HostFs fs;
    fs.read_file = host_read;
    fs.list_dir = host_list;

    g_plan = new fefw_foundation::VfsOverlayPlan();
    std::string err;
    if (!fefw_foundation::plan_vfs_overlays(overlays, fs, *g_plan, &err)) {
        logf(0, "planning failed: %s", err.empty() ? "unknown error" : err.c_str());
        return 1;
    }
    for (const auto& id : g_plan->applied_mods) logf(2, "  mod: %s", id.c_str());
    if (g_plan->applied_mods.empty()) {
        logf(2, "no RuntimePatcher mods installed");
        return 0;
    }
    for (const auto& vpath : g_plan->target_paths) {
        g_targets.push_back(vpath);
        if (!api->edit_file(vpath.c_str() + (vpath[0] == '/' ? 1 : 0), &on_edit, &g_targets.back())) {
            logf(1, "cannot edit %s", vpath.c_str());
        }
    }
    logf(2, "%zu mod(s), %zu file(s) to patch", g_plan->applied_mods.size(), g_targets.size());
    return 0;
}

// Required: a DLL without nxmod_init is unloaded. Nothing to do at the first frame.
NXMOD_EXPORT int nxmod_init(const NxModApi* api, const char* mod_dir) {
    (void)api;
    (void)mod_dir;
    return 0;
}
