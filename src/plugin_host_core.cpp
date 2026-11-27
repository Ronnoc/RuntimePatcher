// SPDX-License-Identifier: MIT
// FEFW RuntimePatcher — platform-independent plugin host core (System menu service + settings store).
#include "fefw_foundation/plugin_host_core.h"

#include <cstdio>
#include <cstring>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace fefw_foundation {

namespace {

#if defined(_MSC_VER) && !defined(__clang__)
inline int atomic_load_acq(const volatile int* p) {
    int v = *p;
    _ReadWriteBarrier();
    return v;
}
inline void atomic_store_rel(volatile int* p, int v) {
    _ReadWriteBarrier();
    *p = v;
}
inline int atomic_add_fetch(volatile int* p, int d) {
    return static_cast<int>(_InterlockedExchangeAdd(reinterpret_cast<volatile long*>(p), d) + d);
}
inline int atomic_sub_fetch(volatile int* p, int d) {
    return static_cast<int>(_InterlockedExchangeAdd(reinterpret_cast<volatile long*>(p), -d) - d);
}
inline int atomic_exchange_acq_rel(volatile int* p, int v) {
    return static_cast<int>(_InterlockedExchange(reinterpret_cast<volatile long*>(p), v));
}
#else
inline int atomic_load_acq(const volatile int* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
inline void atomic_store_rel(volatile int* p, int v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
inline int atomic_add_fetch(volatile int* p, int d) { return __atomic_add_fetch(p, d, __ATOMIC_ACQ_REL); }
inline int atomic_sub_fetch(volatile int* p, int d) { return __atomic_sub_fetch(p, d, __ATOMIC_ACQ_REL); }
inline int atomic_exchange_acq_rel(volatile int* p, int v) { return __atomic_exchange_n(p, v, __ATOMIC_ACQ_REL); }
#endif

template <typename T>
void insertion_sort_by_order(T* items, int n) {
    for (int i = 1; i < n; ++i) {
        T key = items[i];
        int j = i - 1;
        while (j >= 0 && items[j].order > key.order) {
            items[j + 1] = items[j];
            --j;
        }
        items[j + 1] = key;
    }
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// SettingsStore
// ------------------------------------------------------------------------------------------------
void SettingsStore::clear() { count_ = 0; }

SettingsStore::Entry* SettingsStore::find(const char* key) {
    for (int i = 0; i < count_; ++i) {
        if (std::strcmp(entries_[i].key, key) == 0) return &entries_[i];
    }
    return nullptr;
}

const SettingsStore::Entry* SettingsStore::find(const char* key) const {
    for (int i = 0; i < count_; ++i) {
        if (std::strcmp(entries_[i].key, key) == 0) return &entries_[i];
    }
    return nullptr;
}

bool SettingsStore::put(const char* key, size_t klen, const char* value, size_t vlen) {
    if (!key || !value || klen == 0 || klen >= kKeyLen) return false;
    if (vlen >= kValueLen) vlen = kValueLen - 1;
    char k[kKeyLen];
    std::memcpy(k, key, klen);
    k[klen] = 0;
    Entry* e = find(k);
    if (!e) {
        if (count_ >= kMaxEntries) return false;
        e = &entries_[count_++];
        std::memcpy(e->key, k, klen + 1);
    }
    std::memcpy(e->value, value, vlen);
    e->value[vlen] = 0;
    return true;
}

int SettingsStore::get(const char* key, char* out, size_t out_size) const {
    if (!key || !out || !out_size) return -1;
    const Entry* e = find(key);
    if (!e) return -1;
    size_t len = std::strlen(e->value);
    if (len >= out_size) len = out_size - 1;
    std::memcpy(out, e->value, len);
    out[len] = 0;
    return static_cast<int>(len);
}

int SettingsStore::set(const char* key, const char* value) {
    if (!key || !value || !key[0] || std::strlen(key) >= kKeyLen || std::strpbrk(key, "=\r\n") ||
        std::strpbrk(value, "\r\n")) {
        return -1;
    }
    return put(key, std::strlen(key), value, std::strlen(value)) ? 0 : -1;
}

void SettingsStore::parse_ini(const char* data, size_t size) {
    if (!data || !size) return;
    for (size_t i = 0; i < size;) {
        size_t end = i;
        while (end < size && data[end] != '\n') ++end;
        size_t len = end;
        if (len > i && data[len - 1] == '\r') --len;
        const char* eq = static_cast<const char*>(std::memchr(data + i, '=', len - i));
        if (eq && data[i] != '#') {
            put(data + i, static_cast<size_t>(eq - (data + i)), eq + 1,
                static_cast<size_t>((data + len) - (eq + 1)));
        }
        i = end + 1;
    }
}

size_t SettingsStore::format_ini(char* out, size_t out_size) const {
    if (!out || !out_size) return 0;
    size_t n = 0;
    for (int i = 0; i < count_ && n < out_size; ++i) {
        int written = std::snprintf(out + n, out_size - n, "%s=%s\n", entries_[i].key, entries_[i].value);
        if (written < 0) break;
        size_t w = static_cast<size_t>(written);
        if (w >= out_size - n) {
            n = out_size - 1;
            break;
        }
        n += w;
    }
    out[n] = 0;
    return n;
}

void SettingsStore::merge_from(const SettingsStore& overrides) {
    for (int i = 0; i < overrides.count_; ++i) {
        put(overrides.entries_[i].key, std::strlen(overrides.entries_[i].key), overrides.entries_[i].value,
            std::strlen(overrides.entries_[i].value));
    }
}

// ------------------------------------------------------------------------------------------------
// MenuHost
// ------------------------------------------------------------------------------------------------
void MenuHost::lock() const {
    if (plat_.lock_fn) plat_.lock_fn();
}

void MenuHost::unlock() const {
    if (plat_.unlock_fn) plat_.unlock_fn();
}

bool MenuHost::init(const FefwMenuConfig& cfg, const MenuPlatform& plat) {
    if (!plat.main_base || !plat.call_game || !plat.slot_text || !plat.slot_confirm || !plat.slot_left ||
        !plat.slot_right || !plat.slot_dtor[0] || !plat.slot_dtor[1]) {
        ready_ = false;
        return false;
    }
    if (cfg.ui_item_vtable_bytes < 24 || cfg.ui_item_vtable_bytes > kMaxUiItemVtableBytes ||
        (cfg.ui_item_vtable_bytes % 8) != 0) {
        ready_ = false;
        return false;
    }
    const size_t max_slot = (cfg.ui_item_vtable_bytes / 8) - 2;
    if ((cfg.vt_text / 8) >= max_slot || (cfg.vt_confirm / 8) >= max_slot || (cfg.vt_right / 8) >= max_slot ||
        (cfg.vt_left / 8) >= max_slot) {
        ready_ = false;
        return false;
    }
    cfg_ = cfg;
    plat_ = plat;
    ready_ = true;
    return true;
}

void MenuHost::disable() { ready_ = false; }

const char* MenuHost::subgroup_menu_text(void* user, void* item) {
    auto* sg = static_cast<MenuSubgroup*>(user);
    if (sg->title_fn) {
        const char* s = sg->title_fn(sg->title_user, item);
        if (s && s[0]) return s;
    }
    return sg->id;
}

int MenuHost::subgroup_menu_confirm(void* user, void* item) {
    auto* sg = static_cast<MenuSubgroup*>(user);
    void* sub = sg->host->open_submenu(item);
    if (!sub) return FEFW_MENU_IGNORED;
    int n = atomic_load_acq(&sg->count);
    if (n > kMaxCallbacks) n = kMaxCallbacks;
    GroupProvider sorted[kMaxCallbacks];
    for (int i = 0; i < n; ++i) sorted[i] = sg->providers[i];
    insertion_sort_by_order(sorted, n);
    for (int i = 0; i < n; ++i) sorted[i].fn(sorted[i].user, sub);
    return FEFW_MENU_OPENED;
}

void MenuHost::provide_subgroup_entry(void* user, void* menu) {
    auto* sg = static_cast<MenuSubgroup*>(user);
    sg->host->add_entry(menu, &sg->entry);
}

const char* MenuHost::group_menu_text(void* user, void*) { return static_cast<MenuGroup*>(user)->name; }

int MenuHost::group_menu_confirm(void* user, void* item) {
    auto* g = static_cast<MenuGroup*>(user);
    MenuHost* self = g->host;
    int gi = static_cast<int>(g - self->menu_groups_);
    void* sub = self->open_submenu(item);
    if (!sub) return FEFW_MENU_IGNORED;
    int n = atomic_load_acq(&g->count);
    if (n > kMaxCallbacks) n = kMaxCallbacks;
    GroupProvider sorted[kMaxCallbacks + kMaxMenuSubgroups];
    int total = 0;
    for (int i = 0; i < n; ++i) sorted[total++] = g->providers[i];
    int sn = atomic_load_acq(&self->menu_subgroup_count_);
    if (sn > kMaxMenuSubgroups) sn = kMaxMenuSubgroups;
    for (int i = 0; i < sn; ++i) {
        if (self->menu_subgroups_[i].group_idx == gi) {
            GroupProvider p{};
            p.fn = &provide_subgroup_entry;
            p.user = &self->menu_subgroups_[i];
            p.order = self->menu_subgroups_[i].order;
            sorted[total++] = p;
        }
    }
    insertion_sort_by_order(sorted, total);
    for (int i = 0; i < total; ++i) sorted[i].fn(sorted[i].user, sub);
    return FEFW_MENU_OPENED;
}

int MenuHost::register_system_menu_provider(FefwMenuProviderFn fn, void* user) {
    if (!fn) return 0;
    lock();
    int n = menu_provider_count_;
    int ok = n < kMaxCallbacks;
    if (ok) {
        menu_providers_[n] = {fn, user};
        atomic_store_rel(&menu_provider_count_, n + 1);
    }
    unlock();
    return ok;
}

int MenuHost::register_menu_group_provider_ordered(const char* group_name, int order, FefwMenuProviderFn fn,
                                                   void* user) {
    if (!group_name || !group_name[0] || std::strlen(group_name) >= kKeyLen || !fn) return 0;
    lock();
    int gi = -1;
    int gn = menu_group_count_;
    for (int i = 0; i < gn; ++i) {
        if (std::strcmp(menu_groups_[i].name, group_name) == 0) {
            gi = i;
            break;
        }
    }
    int ok = 0;
    if (gi < 0) {
        if (gn < kMaxMenuGroups) {
            gi = gn;
            MenuGroup& g = menu_groups_[gi];
            g.host = this;
            std::snprintf(g.name, sizeof(g.name), "%s", group_name);
            g.providers[0] = {fn, user, order, ""};
            g.entry = {&group_menu_text, &group_menu_confirm, &g};
            atomic_store_rel(&g.count, 1);
            atomic_store_rel(&menu_group_count_, gn + 1);
            ok = 1;
        }
    } else {
        MenuGroup& g = menu_groups_[gi];
        int c = g.count;
        if (c < kMaxCallbacks) {
            g.providers[c] = {fn, user, order, ""};
            atomic_store_rel(&g.count, c + 1);
            ok = 1;
        }
    }
    unlock();
    return ok;
}

int MenuHost::register_menu_group_provider(const char* group_name, FefwMenuProviderFn fn, void* user) {
    return register_menu_group_provider_ordered(group_name, 0, fn, user);
}

int MenuHost::register_menu_subgroup_provider(const char* group_name, const char* sub_id, int sub_order,
                                              const char* (*title_fn)(void* user, void* item), void* title_user,
                                              int item_order, const char* item_key, FefwMenuProviderFn fn, void* user) {
    if (!group_name || !group_name[0] || std::strlen(group_name) >= kKeyLen || !sub_id || !sub_id[0] ||
        std::strlen(sub_id) >= kKeyLen || !fn) {
        return 0;
    }
    if (item_key && std::strlen(item_key) >= kKeyLen) return 0;
    lock();
    int gi = -1;
    int gn = menu_group_count_;
    for (int i = 0; i < gn; ++i) {
        if (std::strcmp(menu_groups_[i].name, group_name) == 0) {
            gi = i;
            break;
        }
    }
    if (gi < 0) {
        if (gn >= kMaxMenuGroups) {
            unlock();
            return 0;
        }
        gi = gn;
        MenuGroup& g = menu_groups_[gi];
        g.host = this;
        std::snprintf(g.name, sizeof(g.name), "%s", group_name);
        g.entry = {&group_menu_text, &group_menu_confirm, &g};
        atomic_store_rel(&g.count, 0);
        atomic_store_rel(&menu_group_count_, gn + 1);
    }
    int si = -1;
    int sn = menu_subgroup_count_;
    for (int i = 0; i < sn; ++i) {
        if (menu_subgroups_[i].group_idx == gi && std::strcmp(menu_subgroups_[i].id, sub_id) == 0) {
            si = i;
            break;
        }
    }
    int ok = 0;
    if (si < 0) {
        if (sn < kMaxMenuSubgroups) {
            si = sn;
            MenuSubgroup& sg = menu_subgroups_[si];
            sg.host = this;
            std::snprintf(sg.id, sizeof(sg.id), "%s", sub_id);
            sg.group_idx = gi;
            sg.order = sub_order;
            sg.title_fn = title_fn;
            sg.title_user = title_user;
            sg.entry = {&subgroup_menu_text, &subgroup_menu_confirm, &sg};
            sg.providers[0] = {fn, user, item_order, ""};
            if (item_key && item_key[0]) std::snprintf(sg.providers[0].item_key, sizeof(sg.providers[0].item_key), "%s", item_key);
            atomic_store_rel(&sg.count, 1);
            atomic_store_rel(&menu_subgroup_count_, sn + 1);
            ok = 1;
        }
    } else {
        MenuSubgroup& sg = menu_subgroups_[si];
        if (sub_order < sg.order) sg.order = sub_order;
        if (!sg.title_fn && title_fn) {
            sg.title_fn = title_fn;
            sg.title_user = title_user;
        }
        int c = sg.count;
        if (item_key && item_key[0]) {
            for (int i = 0; i < c; ++i) {
                if (std::strcmp(sg.providers[i].item_key, item_key) == 0) {
                    unlock();
                    return 1;
                }
            }
        }
        if (c < kMaxCallbacks) {
            GroupProvider& p = sg.providers[c];
            p = {fn, user, item_order, ""};
            if (item_key && item_key[0]) std::snprintf(p.item_key, sizeof(p.item_key), "%s", item_key);
            atomic_store_rel(&sg.count, c + 1);
            ok = 1;
        }
    }
    unlock();
    return ok;
}

int MenuHost::register_menu_closed_callback(FefwMenuClosedFn fn, void* user) {
    if (!fn) return 0;
    lock();
    int n = menu_closed_count_;
    int ok = n < kMaxCallbacks;
    if (ok) {
        menu_closed_[n] = {fn, user};
        atomic_store_rel(&menu_closed_count_, n + 1);
    }
    unlock();
    return ok;
}

void* MenuHost::add_entry(void* menu, const FefwMenuEntry* entry) {
    if (!menu || !entry || !ready_) return nullptr;
    void* heap = **reinterpret_cast<void***>(plat_.main_base + cfg_.ui_heap_got);
    const uint64_t alloc_args[4] = {reinterpret_cast<uint64_t>(heap), sizeof(PluginMenuItem), 16, 0};
    auto* item =
        reinterpret_cast<PluginMenuItem*>(plat_.call_game(plat_.main_base + cfg_.ui_alloc, alloc_args, 4));
    if (!item) return nullptr;
    std::memset(item->native, 0, sizeof(item->native));
    std::memcpy(item->table, reinterpret_cast<const void*>(plat_.main_base + cfg_.ui_item_vtable),
                cfg_.ui_item_vtable_bytes);
    uint64_t* vt = item->table + 2;  // skip offset-to-top + typeinfo
    vt[cfg_.vt_text / 8] = plat_.slot_text;
    vt[cfg_.vt_confirm / 8] = plat_.slot_confirm;
    vt[cfg_.vt_right / 8] = plat_.slot_right;
    vt[cfg_.vt_left / 8] = plat_.slot_left;
    orig_dtor_[0] = vt[0];
    orig_dtor_[1] = vt[1];
    vt[0] = plat_.slot_dtor[0];
    vt[1] = plat_.slot_dtor[1];
    atomic_add_fetch(&live_rows_, 1);
    *reinterpret_cast<uint64_t**>(item->native) = vt;
    *reinterpret_cast<uint32_t*>(item->native + 0x50) = 0x00010101;  // visible / enabled flags
    item->owner = item;
    item->entry = entry;
    item->step = nullptr;
    const uint64_t add_args[2] = {reinterpret_cast<uint64_t>(menu), reinterpret_cast<uint64_t>(item)};
    plat_.call_game(plat_.main_base + cfg_.ui_add_item, add_args, 2);
    return item;
}

void* MenuHost::open_submenu(void* item) {
    if (!item || !ready_) return nullptr;
    void* parent = *reinterpret_cast<void**>(static_cast<uint8_t*>(item) + 8);
    if (parent) {
        void* bound = *reinterpret_cast<void**>(static_cast<uint8_t*>(parent) + 0x98);
        if (bound) parent = bound;
    }
    if (!parent) return nullptr;
    uint64_t info[3] = {reinterpret_cast<uint64_t>(item), 1, 0};
    const uint64_t args[2] = {reinterpret_cast<uint64_t>(parent), reinterpret_cast<uint64_t>(info)};
    return reinterpret_cast<void*>(plat_.call_game(plat_.main_base + cfg_.ui_submenu, args, 2));
}

void MenuHost::refresh(void* item) {
    if (!item || !ready_) return;
    const uint64_t arg = reinterpret_cast<uint64_t>(item);
    plat_.call_game(plat_.main_base + cfg_.ui_refresh, &arg, 1);
}

int MenuHost::set_step(void* item, FefwMenuStepFn fn) {
    if (!item) return -1;
    static_cast<PluginMenuItem*>(item)->step = fn;
    return 0;
}

void MenuHost::on_system_menu(void* menu) {
    if (!ready_ || !menu) return;
    int n = atomic_load_acq(&menu_provider_count_);
    if (n > kMaxCallbacks) n = kMaxCallbacks;
    for (int i = 0; i < n; ++i) menu_providers_[i].fn(menu_providers_[i].user, menu);
    int gn = atomic_load_acq(&menu_group_count_);
    if (gn > kMaxMenuGroups) gn = kMaxMenuGroups;
    for (int i = 0; i < gn; ++i) add_entry(menu, &menu_groups_[i].entry);
}

void MenuHost::flush_menu_closed() {
    if (atomic_exchange_acq_rel(&menu_closed_pending_, 0)) {
        int m = atomic_load_acq(&menu_closed_count_);
        if (m > kMaxCallbacks) m = kMaxCallbacks;
        for (int i = 0; i < m; ++i) menu_closed_[i].fn(menu_closed_[i].user);
    }
}

const char* MenuHost::handle_text(PluginMenuItem* item) {
    const FefwMenuEntry* e = item ? item->entry : nullptr;
    const char* s = (e && e->text) ? e->text(e->user, item) : nullptr;
    return s ? s : "";
}

int MenuHost::handle_confirm(PluginMenuItem* item) {
    if (!item || item->native[0x52] != 1) return FEFW_MENU_IGNORED;  // row disabled
    const FefwMenuEntry* e = item->entry;
    return (e && e->confirm) ? e->confirm(e->user, item) : FEFW_MENU_IGNORED;
}

int MenuHost::handle_step(PluginMenuItem* item, int dir) {
    if (!item || item->native[0x52] != 1 || !item->step) return 0;
    const FefwMenuEntry* e = item->entry;
    return item->step(e ? e->user : nullptr, item, dir) ? 1 : 0;
}

void MenuHost::handle_dtor(PluginMenuItem* item, int which) {
    if (atomic_sub_fetch(&live_rows_, 1) == 0) {
        atomic_store_rel(&menu_closed_pending_, 1);
    }
    if (which >= 0 && which < 2 && orig_dtor_[which] && plat_.call_game) {
        const uint64_t arg = reinterpret_cast<uint64_t>(item);
        plat_.call_game(static_cast<uintptr_t>(orig_dtor_[which]), &arg, 1);  // which = 1 frees `item`
    }
}

int MenuHost::live_rows() const { return atomic_load_acq(&live_rows_); }

bool MenuHost::has_menu_closed_callbacks() const { return atomic_load_acq(&menu_closed_count_) > 0; }

}  // namespace fefw_foundation
