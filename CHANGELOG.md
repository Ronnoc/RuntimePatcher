# Changelog

All notable changes to this project are documented here. Versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.1.1]

First public release (replaces the withdrawn 0.1.0).

- Runtime RomFS data overlay: `Job.bin`, `Skill.bin`, `AssetTable.bin`, other GameData tables, MSBT messages and icon
  archives are patched in memory from mod manifests; mods need no merged files.
- NRO loader (default) and static `subsdk9` loader; native code plugins for mods.
- Game-version-specific values come from a game profile (your game files + hooks file, or a saved profile), never
  from this repository; see `docs/GAME_PROFILE.md`.
- Mod-specific data (class icons, skill icons, enemy stat scaling) comes from each mod's `manifest.json`.
- Build/deploy tool for Windows, Linux and macOS; deploy only replaces files it created.
- Version shown in the runtime's load log line (`[FEFW] RuntimePatcher 0.1.1 (NRO) loaded`).
- Third-party credits and licences in `THIRD_PARTY_NOTICES.md`, included in release zips.
