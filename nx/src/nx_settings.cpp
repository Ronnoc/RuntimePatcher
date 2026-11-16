// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — persistent plugin settings (NRO loader only).
//
// One small text file in the game's own save data, `<mount>:/fefw_settings.ini`, one `key=value` per line.
// The game mounts its save data itself; until it has (early boot), reads report "not ready" and writes are kept
// in memory and written on a later frame. Writes are committed right away (nn::fs::Commit), so the file survives
// a crash and never waits for the next in-game save. Game thread only (menu / frame callbacks).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
#endif
}  // namespace fs
}  // namespace nn

namespace {

constexpr const char* kMount = "save";  // the game's save data mount (it commits with nn::fs::Commit("save"))
constexpr const char* kPath = "save:/fefw_settings.ini";
constexpr int kMaxEntries = 64;
constexpr int kKeyLen = 64;
constexpr int kValueLen = 128;
constexpr int kOpenRead = 1, kOpenWrite = 2 | 4;  // Write | AllowAppend

struct Entry {
    char key[kKeyLen];
    char value[kValueLen];
};
Entry g_entries[kMaxEntries];
int g_count = 0;
bool g_loaded = false;
bool g_dirty = false;
int g_failures = 0;  // write attempts that failed; gives up after a few (no log spam every frame)

bool mount_ready() {
#if FEFW_NX_NRO
    // Not GetEntryType("save:/"): on an unknown mount name nn::fs aborts the game (auto-abort policy).
    return nn::fs::IsMounted(kMount);
#else
    return false;
#endif
}

Entry* find(const char* key) {
    for (int i = 0; i < g_count; ++i)
        if (strcmp(g_entries[i].key, key) == 0) return &g_entries[i];
    return nullptr;
}

void put(const char* key, size_t klen, const char* value, size_t vlen) {
    if (!klen || klen >= kKeyLen) return;
    if (vlen >= kValueLen) vlen = kValueLen - 1;
    char k[kKeyLen];
    memcpy(k, key, klen);
    k[klen] = 0;
    Entry* e = find(k);
    if (!e) {
        if (g_count >= kMaxEntries) return;
        e = &g_entries[g_count++];
        memcpy(e->key, k, klen + 1);
    }
    memcpy(e->value, value, vlen);
    e->value[vlen] = 0;
}

// Loads the file once the mount exists. Values set before that (kept in memory) win over the file.
bool ensure_loaded() {
    if (g_loaded) return true;
    if (!mount_ready()) return false;
    g_loaded = true;
#if FEFW_NX_NRO
    nn::fs::DirectoryEntryType t{};
    if (nn::fs::GetEntryType(&t, kPath) != 0) return true;  // no file yet (PathNotFound does not abort)
    nn::fs::FileHandle h{};
    if (nn::fs::OpenFile(&h, kPath, kOpenRead) != 0) return true;
    static char buf[kMaxEntries * (kKeyLen + kValueLen + 2)];
    int64_t size = 0;
    size_t got = 0;
    if (nn::fs::GetFileSize(&size, h) == 0 && size > 0) {
        if (size > static_cast<int64_t>(sizeof(buf))) size = sizeof(buf);
        if (nn::fs::ReadFile(&got, h, 0, buf, static_cast<size_t>(size)) != 0) got = 0;
    }
    nn::fs::CloseFile(h);
    Entry pending[kMaxEntries];
    int npending = g_count;
    memcpy(pending, g_entries, sizeof(Entry) * g_count);
    g_count = 0;
    for (size_t i = 0; i < got;) {
        size_t end = i;
        while (end < got && buf[end] != '\n') ++end;
        size_t len = end;
        if (len > i && buf[len - 1] == '\r') --len;
        const char* eq = static_cast<const char*>(memchr(buf + i, '=', len - i));
        if (eq && buf[i] != '#') put(buf + i, eq - (buf + i), eq + 1, buf + len - (eq + 1));
        i = end + 1;
    }
    for (int i = 0; i < npending; ++i)
        put(pending[i].key, strlen(pending[i].key), pending[i].value, strlen(pending[i].value));
    fefw_nx::log("settings: %d value(s) from %s", g_count, kPath);
#endif
    return true;
}

bool write_file() {
#if FEFW_NX_NRO
    static char buf[kMaxEntries * (kKeyLen + kValueLen + 2)];
    size_t n = 0;
    for (int i = 0; i < g_count; ++i)
        n += snprintf(buf + n, sizeof(buf) - n, "%s=%s\n", g_entries[i].key, g_entries[i].value);
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
    const Entry* e = find(key);
    if (!e) return -1;
    size_t len = strlen(e->value);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, e->value, len);
    out[len] = 0;
    return static_cast<int>(len);
}

int fefw_nx::settings_set(const char* key, const char* value) {
    if (!key || !value || strchr(key, '=') || strchr(key, '\n') || strchr(value, '\n')) return -1;
    ensure_loaded();
    put(key, strlen(key), value, strlen(value));
    g_dirty = true;
    g_failures = 0;
    settings_flush();
    return 0;
}

void fefw_nx::settings_flush() {
    if (!g_dirty || g_failures >= 3 || !ensure_loaded()) return;  // before the mount exists: retried each frame
    if (write_file()) {
        g_dirty = false;
        g_failures = 0;
    } else {
        ++g_failures;
    }
}
