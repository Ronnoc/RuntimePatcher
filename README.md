# FEFW RuntimePatcher

[![CI](https://github.com/Ronnoc/RuntimePatcher/actions/workflows/ci.yml/badge.svg)](https://github.com/Ronnoc/RuntimePatcher/actions/workflows/ci.yml)

A runtime data patcher for **_Fire Emblem: Fortune's Weave_** on Switch 2 emulators (Eden / Citron).

It loads after boot, hooks the game's file access and patches game data in memory, so mods ship small
declarative deltas instead of whole replacement `GameData/*.bin`, `Msg/*/*.msbt.zst` or `Layout/Textures/*.arc.zst` files.

> **Disclaimer.** This is an unofficial fan project, not affiliated with or endorsed by Nintendo or Intelligent
> Systems. It contains no game code or assets. Use it only with a copy of the game you own, dumped from your own
> console, and don't share game files. Provided as is, without warranty: back up your saves. See [LEGAL.md](LEGAL.md).

## Install (prebuilt)

Download `FEFW_RuntimePatcher-<version>.zip` from [Releases](https://github.com/Ronnoc/RuntimePatcher/releases),
unzip it into `<emulator>/load/04007EF01F8AE000/` and enable `FEFW_RuntimePatcher` in the emulator's add-on list.
Each release is built for one game version (noted in the release); other versions need a source build.

## Features

- **Multiple mods side by side, no merging.** Each mod ships a `manifest.json`, packaged as
  `romfs/GameData/Classes/<mod_id>.delta|.str|.bntx`. At runtime `Job.bin`, `Skill.bin`, `AssetTable.bin`,
  `Msg/<lang>/*.msbt.zst` and `Layout/Textures/Icon_*.arc.zst` are read from the active RomFS (including any
  other mod's replacement files), patched and served from memory.
- **Vanilla data stays intact.** New table rows are appended; existing rows and indices are never moved.
- **No game file changes.** The default loader is a small `main` IPS that loads `runtime_patcher.nro` through
  the game's own `nn::ro`. A static `subsdk9` loader is available as a fallback.
- **Plugins.** Mods can ship native code plugins (see [`include/fefw_foundation/plugin_api.h`](include/fefw_foundation/plugin_api.h)).
- **Host library.** The C++20 core (`fefw_class_foundation`) also builds on PC for tooling, e.g. `fefw_table_dump`.

---

## Quick start (tested on Eden)

Prerequisites: Python 3.10+ (`pip install zbic lz4`), LLVM (`clang`, `ld.lld`; `winget install LLVM.LLVM`), CMake 3.25+ (host library only),
your own extracted game `exefs` (set `FEFW_GAME_EXEFS` or pass `--game-exefs`) and a hooks file
(`FEFW_GAME_HOOKS` / `--hooks`), or a saved game profile instead of both (`FEFW_GAME_PROFILE` / `--profile`).

```
python tools/fetch_third_party.py                 # once: musl + libc++ headers
python tools/build_emulator_mod.py all --deploy   # build foundation (NRO loader) + mods in ./mods, install
```

- **NRO loader (default):** `FEFW_RuntimePatcher` ships `exefs/<main build id>.ips` (redirects main's `nn::fs` PLT
  entries to a small bootstrap) + `romfs/fefw/runtime_patcher.nro|nrr`, loaded after boot through the game's own `nn::ro`.
  No game exefs changes, no rtld patch.
- **Static loader (fallback):** `--loader static` installs `subsdk9` into the game's `exefs/`.
- `--deploy` copies the packages into `<emulator user folder>/load/04007EF01F8AE000/` for every installed
  emulator (`%APPDATA%/eden` on Windows, `~/.local/share/eden` on Linux, `~/Library/Application Support/eden`
  on macOS; same for citron and others). Set `FEFW_EMU_DIR` to choose the folders yourself.
- Eden log filter: `*:Info Debug.Emulated:Debug` to see `[FEFW]` / `[FEFW-BOOT]` messages.
- Targets: `all` | `foundation` | `mods` | `probe` (NRO diagnostic). `--clean` wipes `build/nx`.

Mods kept outside this repo (e.g. when it is used as a git submodule):

```
python tools/build_emulator_mod.py all --mods-dir ../my_mods --out-dir .. --deploy
```

`FEFW_MODS_DIR` / `FEFW_OUT_DIR` / `FEFW_GAME_EXEFS` env vars do the same.

Host library (also usable via `add_subdirectory`, target `fefw_class_foundation`):

```
cmake -S . -B build/host && cmake --build build/host --config Release
ctest --test-dir build/host -C Release       # unit tests (no game data needed)
```

To add a new class, follow [`docs/ADDING_A_NEW_CLASS.md`](docs/ADDING_A_NEW_CLASS.md).

---

## Repository layout

```
include/fefw_foundation/   public C++ API (GameTable, MSBT, BNTX patchers, VFS overlay)
src/                       portable C++20 implementation (host + Switch)
nx/                        runtime: crt0.S (MOD0, NRO header gap), link.ld, src/nx_fs_hook.cpp (nn::fs hooks,
                           static + NRO), src/nx_runtime.cpp (svc glue, sdk lookup), src/nx_cxx_support.cpp
nx/boot/                   NRO bootstrap placed in main: PLT redirect stubs + nn::ro loader (boot_stubs.S, boot.c)
nx/probe/                  minimal NRO used by the `probe` target
mods/<id>/                 your mods (not included): manifest.json, resources/, optional plugin src/
tools/build_emulator_mod.py  cross-compile the runtime, package mods, deploy
tools/fefw_delta.py        .delta / .str / .bntx package writer (format read by src/vfs_overlay.cpp)
tools/nso_tool.py          NSO/NRO utility: elf2nso, elf2nro, mknrr (Switch 2 NRR), dynamic symbol dump
tools/nro_bootstrap.py     builds the main bootstrap IPS (PLT/GOT analysis, nx/boot link, IPS32)
tools/fetch_third_party.py fetches pinned musl / libc++ headers into third_party/
tests/unit_tests.cpp       host unit tests (ctest)
docs/ADDING_A_NEW_CLASS.md step-by-step class authoring guide + pitfalls
docs/TABLE_FORMAT.md       GameData table (.bin) container format
docs/GAME_PROFILE.md       game-version-specific values: hooks file, saved profile, CI secret
tools/game_profile.py      game profile helpers (encode a profile for the CI secret)
```

Generated (git-ignored): `build/` (`build/nx` module objects + `gen/fefw_sdk_offsets.h`, `build/host` CMake),
`release/emulator_ready/` (packages), `third_party/`.

---

## Game profile

Addresses that depend on the game version are not in this repository. A build reads them from your game files plus
a hooks file, or from a saved profile: see [docs/GAME_PROFILE.md](docs/GAME_PROFILE.md).

---

## Versioning and releases

The version lives in [`VERSION`](VERSION) ([SemVer](https://semver.org/)). It is compiled into the runtime and
shown in its load log line (`[FEFW] RuntimePatcher 0.1.1 (NRO) loaded`).

To release:

1. Bump `VERSION` and move the `Unreleased` notes in [`CHANGELOG.md`](CHANGELOG.md) under a `## [x.y.z]` heading.
2. Commit, then tag and push: `git tag vx.y.z && git push origin main vx.y.z`.
3. The Release workflow checks the tag matches `VERSION`, runs the tests, builds the runtime from the
   `FEFW_GAME_PROFILE` repository secret and publishes a GitHub release with the runtime zip, the source archive
   and that version's changelog. Versions like `1.0.0-rc.1` become pre-releases.

Before the first release, and after every game update, refresh the secret (see
[docs/GAME_PROFILE.md](docs/GAME_PROFILE.md)). Running the Release workflow by hand (Actions tab) builds the zip as
a workflow artifact without publishing anything.

---

## License

MIT, see [LICENSE](LICENSE). Third-party code and credits: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
Trademarks, what releases contain and how to contact the maintainer:
[LEGAL.md](LEGAL.md).
