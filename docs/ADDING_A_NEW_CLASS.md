# Adding a New Class (Eden / Citron)

Tested on Eden. The "Pitfalls" section lists failures seen during development.

> **Never** ship or generate a merged `Job.bin` / `Skill.bin` / `AssetTable.bin`. Class mods contain only
> `romfs/GameData/Classes/<id>.json` (plain-text JSON shared by PC and emulator; legacy `.delta` is still
> accepted at runtime); the `subsdk9` foundation module synthesizes the
> patched tables in memory at runtime, on top of whatever base (or third-party) `Job.bin` is loaded.

---

## 1. How it works at runtime

Two loaders share the same foundation code (`src/` + `nx/src/`). **NRO is the default** (works next to
other code mods unmodified); static is the fallback.

### NRO loader (default, `--loader nro`)

```
rtld ── main* ── subsdk0..3 ── subsdk9 ── sdk   (* main patched by FEFW_RuntimePatcher/exefs/<build id>.ips;
                                  │               subsdk9 = FEFW_RuntimePatcher/exefs/subsdk9, the bootstrap)
                                  └─ nn::ro loads romfs:/fefw/runtime_patcher.nro after boot
```

1. The IPS rewrites main's PLT entries for `nn::fs::{OpenFile, ReadFile, ReadFile(size_t*), GetFileSize,
   CloseFile}` to `b fefw_boot_stub_N`, and places `nx/boot` (asm dispatcher + freestanding C loader) in
   main's unused `.text` tail.
2. Each call asks `nn::ro::LookupSymbol("fefw_fs_*")`. Not found -> it runs an exact copy of the original
   PLT entry. main uses **lazy binding** (GOT slots point at PLT0 until first use), so never jump to the GOT
   value directly.
3. Once main's OpenFile GOT slot is bound, the first `OpenFile` on `Contents:` reads `runtime_patcher.nro` +
   `runtime_patcher.nrr` and calls `nn::ro::Initialize / RegisterModuleInfo / LoadModule`. From then on every
   stub forwards to the NRO's `fefw_fs_*` hooks.
4. The NRO imports the real `nn::fs` functions normally (bound by `nn::ro` at load), so it needs no symbol
   lookup tricks. Static modules, the game exefs and rtld are untouched.

NRR format: this game's sdk only accepts the Switch 2 layout (`nso_tool.py mknrr --format 1`, the default):
bytes 0x05 / 0x06 / 0x20 = 1, 0x180-byte key/signature blocks, body at 0x4B0, hashes at 0x4D0. The Switch 1
layout is rejected with `0xC16` (ro `InvalidNrr`) by the sdk's own parser.

### Static loader (fallback, `--loader static`)

```
rtld ── main ── subsdk0..N ── subsdk9 (FEFW foundation) ── sdk
```

`subsdk9` exports the `nn::fs` symbols `main` imports, so rtld binds `main` to it. Inserting it shifts
`sdk`. Real sdk
entry points come from build-time offsets (`build/nx/gen/fefw_sdk_offsets.h`) because the runtime dynsym
lookup fails on Eden NX2.

### Both

On the first open of a file in the RomFS mount that contains `GameData/Job.bin`, the foundation scans
`<mount>:/GameData/Classes/` (LayeredFS merges every class mod's `romfs/` there) and runs
`fefw_foundation::apply_vfs_overlays`. Reads of `Job.bin`, `Skill.bin`, `AssetTable.bin`,
`Msg/*/Job|Skill.msbt.zst` and `Layout/Textures/Icon_Job|Icon_Skill.arc.zst` are then served from memory.
Game addresses come from the game profile (your game files + a hooks file, or a saved profile; see
`tools/game_profile.py`): always rebuild against the game version being played.

---

## 2. Create the class mod

1. Create the mod folder:
   ```
   mods/<mod_id>/
     manifest.json      <- the only input the build reads (plus resources/*.swizzled)
     resources/         <- optional custom textures (*.swizzled), copied into the package
                           (another folder: "resources_dir": "<path relative to manifest.json>")
     README.md          <- design notes for humans
   ```
2. Edit `manifest.json`:
   - `id` (lowercase, unique; becomes package `FEFW_<Id>_Mod` and `romfs/GameData/Classes/<id>.json`), `enabled: true`.
   - `classes[]` — one entry per class row (a mounted class needs its dismounted row too):
     - `key` new `JID_…` (unique), `clone_base` existing `JID_…` row to clone, `name_id`/`help_id` (`MJID_…`).
     - `rank`, `system` (weapon line), `gender`, `movement_type`, `mount_rid`, `dismount_target`.
     - `lower_class` / `upper_class` and `relink_progression` (patches the neighbouring vanilla rows so the
       new class appears in the promotion tree).
     - `base_stats`, `stat_modifiers`, `growths`, `weapon_exp`, `innate_skills`, `attribute_skills`,
       `mastery_skills`, `exam` (seal, display weapon, requirements, stat minimums), `assets`
       (`clone_from` row for AssetTable clauses, `model_aid`, `icon_name`).
   - `custom_skills[]` (optional): `key` `SID_…`, `clone_from`, `timing`, `stance`, `buffs`, `arg0`, icon
     (`proto_icon` = reuse an existing icon, or `icon_texture` = file in `resources/` with `icon_clone_from` =
     the stock `IconSkill_…` entry used as template, default `IconSkill_SID_はやぶさ`), `localization`.
   - `enemy_scalings` (per class, optional): 11 enemy stat scaling values; without it the cloned row's values stay.
   - `job_icons[]` (optional): class icon entries, typically six per class (`<JID>女`, `<JID>男`, `<JID>` and
     their `_Clr` variants): `{"key": "JID_…女", "clone_from": "JID_<stock class>女", "texture": "x_female.swizzled"}`.
     `texture` may be a list of files; the first one present in `resources/` is used. With no texture the donor icon
     is cloned unchanged.
   - `localization` for class name/help: provide all 12 languages (`CNzh TWzh JPja USen EUen USfr EUfr USes
     EUes EUde EUit KRko`). May be placed at the manifest root (shared by all entries in `classes[]`) or
     inside each `classes[]` object individually (for multi-class packs; falls back to root `localization`).
3. Look up existing keys by dumping the game tables with `fefw_table_dump` (built by the host CMake project).
4. Table rules:
   - rows are only ever **appended**; never reuse/rename a vanilla `JID_`.
   - `ref` fields take `null` or an existing key, never `""`.
   - dismounted forms: no exam seal, system category 0, registered in AssetTable like the mounted form.

---

## 3. Build & deploy

From the repo root (first time only: `python tools/fetch_third_party.py`):

```
python tools/build_emulator_mod.py all --deploy                                      # NRO loader (default)
python tools/build_emulator_mod.py all --deploy --loader static                      # static fallback
```

What `--deploy` does:
- packages land in `release/emulator_ready/<title id>/` (`FEFW_RuntimePatcher` + one `FEFW_<Id>_Mod` per
  manifest) and are copied into `<emulator user folder>/load/<title id>/` (see README; `FEFW_EMU_DIR` overrides);
- **nro**: `FEFW_RuntimePatcher` = `exefs/<main build id>.ips` + `exefs/subsdk9` (bootstrap) +
  `romfs/fefw/runtime_patcher.nro|nrr`; copies the bootstrap `subsdk9` into the game exefs (`FEFW_GAME_EXEFS`; Eden does not layer new exefs files from `load/`) (`FEFW_GAME_EXEFS`); checks that our main IPS does not overlap other mods' main IPS;
- **static**: installs `subsdk9` into the game exefs;
- writes the deployed mode to `load/<title id>/.fefw_loader` and prints `[!] loader switched …` when it
  changes. Both modes write the same folders; the last deploy wins.

In Eden's add-on list make sure `FEFW_RuntimePatcher` and every `FEFW_<Id>_Mod` are enabled.

---

## 4. Verify

1. Eden log filter must include `Debug.Emulated:Debug` (exact spelling; `Debug_Emulated` is rejected).
   `<eden user folder>/config/qt-config.ini` → `log_filter="*:Info Debug.Emulated:Debug"`.
2. Boot the game, then check `<eden user folder>/log/eden_log.txt`. NRO loader:
   ```
   [FEFW-BOOT] loading runtime_patcher.nro
   [FEFW-BOOT] nn::ro::Initialize 0x00000000
   [FEFW-BOOT] nn::ro::RegisterModuleInfo 0x00000000
   [FEFW] RuntimePatcher (NRO) loaded
   [FEFW-BOOT] nn::ro::LoadModule 0x00000000
   [FEFW] nn::fs hooks active (NRO, real fs bound by nn::ro)
   ```
   Static loader instead: `[FEFW] RuntimePatcher (subsdk9) loaded`, `[FEFW] sdk located at …`,
   `[FEFW] nn::fs interposer active …`. Then, for both:
   ```
   [FEFW] building class overlay from Contents:/GameData/Classes ...
   [FEFW]   mod: <id>
   [FEFW]   synthesized /GameData/Job.bin (... bytes)
   [FEFW] class overlay ready: N mod(s), M file(s)
   [FEFW] serving synthesized Contents:/GameData/Job.bin
   ```
   `N` counts only class mods **enabled** in Eden's add-on list.
3. In game: class change menu lists the new class(es).

---

## 5. Pitfalls (all observed)

| Symptom | Cause | Fix |
| --- | --- | --- |
| Static loader: classes missing, log has no `subsdk9` line | the emulator does not layer *new* NSOs from `load/` (older yuzu forks only replace existing ones) | install subsdk9 into the game exefs (deploy does it) |
| NRO loader: game crashes right at boot | `exefs/subsdk9` (bootstrap) missing or not layered in, while the main IPS branches into it | ship the IPS and `subsdk9` together; on Eden it must be in the game exefs (deploy copies it there) |
| Crash `Unmapped read 0x0` after `[FEFW] ERROR: cannot resolve real _ZN2nn2fs…` | runtime dynsym lookup fails on Eden NX2 | build-time sdk offsets fallback (already in place); rebuild if the game's `sdk` changes |
| NRO: `[FEFW-BOOT] nn::ro::RegisterModuleInfo 0x00000c16` | NRR in Switch 1 layout | `mknrr --format 1` (default) |
| NRO: crash right after boot, PC in main's rodata, return address inside the bootstrap | stub used an unbound (lazy) GOT value as a function address | fall back through the copied PLT entry (`fefw_boot_orig_N`); touch `nn::ro` only after the OpenFile slot is bound (in place) |
| Loader mode flips between test runs | another checkout deployed the other loader | check `load/…/.fefw_loader` / the `[!] loader switched` line; deploy from one place only |
| Eden looks stuck on "launching" | the game crashed; Eden keeps the window | close Eden, read the log |
| No `[FEFW]` lines at all | log filter misspelled | `Debug.Emulated:Debug` |
