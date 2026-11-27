// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — persistent plugin settings (NRO loader only).
//
// One small text file in the game's own save data, `<mount>:/fefw_settings.ini`, one `key=value` per line.
// The game mounts its save data only while it reads or writes it (it mounts and unmounts "save" many times). So the
// file is read the first time the mount is there, and a changed value is kept in memory and written on the first
// frame the mount is there again (the next time the game touches its save). nn::fs auto-abort is off around every
// call, so a mount that goes away between the check and the call is a logged error, not a crash. Writes are committed
// right away (nn::fs::Commit), so the file survives a crash and never waits for the next in-game save. Game thread
// only (menu / frame callbacks).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fefw_foundation/plugin_host_core.h"
#include "nx_runtime.h"
#include "nx_settings.h"

namespace nn {
namespace fs {
struct FileHandle {
    void* handle;
};
enum DirectoryEntryType : int {};
struct WriteOption {
    int flags;  // 1 = flush
};
#if FEFW_NX_NRO
uint32_t OpenFile(FileHandle*, const char*, int);
uint32_t ReadFile(size_t*, FileHandle, int64_t, void*, size_t);
uint32_t WriteFile(FileHandle, int64_t, const void*, size_t, const WriteOption&);
uint32_t GetFileSize(int64_t*, FileHandle);
uint32_t SetFileSize(FileHandle, int64_t);
void CloseFile(FileHandle);
uint32_t CreateFile(const char*, int64_t);
uint32_t GetEntryType(DirectoryEntryType*, const char*);
uint32_t Commit(const char*);
bool IsMounted(const char*);
void SetEnabledAutoAbort(bool);
#endif
}  // namespace fs
}  // namespace nn

namespace {

constexpr const char* kMount = "save";  // the game's save data mount (it commits with nn::fs::Commit("save"))
constexpr const char* kPath = "save:/fefw_settings.ini";
constexpr int kOpenRead = 1, kOpenWrite = 2 | 4;  // Write | AllowAppend

fefw_foundation::SettingsStore g_store;
bool g_loaded = false;
bool g_dirty = false;
int g_failures = 0;  // write attempts that failed; gives up after a few (no log spam every frame)

struct NoAutoAbort {  // the sdk's ScopedAutoAbortDisabler has no exported destructor
#if FEFW_NX_NRO
    NoAutoAbort() { nn::fs::SetEnabledAutoAbort(false); }
    ~NoAutoAbort() { nn::fs::SetEnabledAutoAbort(true); }
#endif
};

bool mount_ready() {
#if FEFW_NX_NRO
    // Not GetEntryType("save:/"): on an unknown mount name nn::fs aborts the game (auto-abort policy).
    return nn::fs::IsMounted(kMount);
#else
    return false;
#endif
}

// Loads the file once the mount exists. Values set before that (kept in memory) win over the file.
bool ensure_loaded() {
    if (g_loaded) return true;
    if (!mount_ready()) return false;
#if FEFW_NX_NRO
    NoAutoAbort guard;
    nn::fs::DirectoryEntryType t{};
    uint32_t er = nn::fs::GetEntryType(&t, kPath);
    if (er != 0 && !mount_ready()) return false;  // unmounted under us: try again later
    g_loaded = true;
    if (er != 0) return true;  // no file yet

    nn::fs::FileHandle h{};
    if (nn::fs::OpenFile(&h, kPath, kOpenRead) != 0) return true;
    static char buf[fefw_foundation::SettingsStore::kMaxIniBytes];
    int64_t size = 0;
    size_t got = 0;
    if (nn::fs::GetFileSize(&size, h) == 0 && size > 0) {
        if (size > static_cast<int64_t>(sizeof(buf))) size = sizeof(buf);
        if (nn::fs::ReadFile(&got, h, 0, buf, static_cast<size_t>(size)) != 0) got = 0;
    }
    nn::fs::CloseFile(h);
    static fefw_foundation::SettingsStore pending;
    pending = g_store;
    g_store.clear();
    g_store.parse_ini(buf, got);
    g_store.merge_from(pending);
    fefw_nx::log("settings: %d value(s) from %s", g_store.count(), kPath);
#else
    g_loaded = true;
#endif
    return true;
}

bool write_file() {
#if FEFW_NX_NRO
    NoAutoAbort guard;
    static char buf[fefw_foundation::SettingsStore::kMaxIniBytes];
    size_t n = g_store.format_ini(buf, sizeof(buf));
    nn::fs::DirectoryEntryType t{};
    if (nn::fs::GetEntryType(&t, kPath) != 0 && nn::fs::CreateFile(kPath, 0) != 0) {
        fefw_nx::log("settings: cannot create %s", kPath);
        return false;
    }
    nn::fs::FileHandle h{};
    uint32_t r = nn::fs::OpenFile(&h, kPath, kOpenWrite);
    if (r != 0) {
        fefw_nx::log("settings: cannot open %s for writing (0x%x)", kPath, r);
        return false;
    }
    nn::fs::WriteOption opt{1};
    r = nn::fs::SetFileSize(h, static_cast<int64_t>(n));
    if (r == 0 && n) r = nn::fs::WriteFile(h, 0, buf, n, opt);
    nn::fs::CloseFile(h);
    if (r == 0) r = nn::fs::Commit(kMount);
    if (r != 0) {
        fefw_nx::log("settings: writing %s failed (0x%x)", kPath, r);
        return false;
    }
    return true;
#else
    return false;
#endif
}

}  // namespace

int fefw_nx::settings_get(const char* key, char* out, size_t out_size) {
    if (!key || !out || !out_size) return -1;
    if (!ensure_loaded()) return FEFW_SETTINGS_NOT_READY;
    return g_store.get(key, out, out_size);
}

int fefw_nx::settings_set(const char* key, const char* value) {
    if (!key || !value) return -1;
    ensure_loaded();
    if (g_store.set(key, value) != 0) return -1;
    g_dirty = true;
    g_failures = 0;
    settings_flush();
    return 0;
}

void fefw_nx::settings_flush() {
    // retried each frame until the game has its save mounted again
    if (!g_dirty || g_failures >= 3 || !ensure_loaded() || !mount_ready()) return;
    if (write_file()) {
        g_dirty = false;
        g_failures = 0;
    } else {
        ++g_failures;
    }
}
