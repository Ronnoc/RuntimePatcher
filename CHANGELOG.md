# Changelog

All notable changes to this project are documented here. Versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

- New table op `APPEND_MATRIX_ROW`: appends a row to a square "row x row" sheet (one array entry per row, in row
  order) and pads every existing row's array with its pair value (`pair`: `default` + per-key values), so several
  mods can each add a row.
- `APPEND_ROW` takes `"allow_duplicate": true` to skip its column-0 duplicate check (keyless continuation rows
  that legitimately repeat column 0, e.g. `[]`).
- `APPEND_ROW` takes `"clone_from_where": {"column": N, "equals": "..."}`: the template is the first row whose
  string column equals the value, and the new row goes right after it (keeps sectioned sheets such as Variable in order).
- Shared System menu groups: `register_menu_group_provider(group_name, fn, user)` and
  `register_menu_group_provider_ordered(group_name, order, fn, user)` (appended to plugin API version 3)
  merge providers from separate plugins that share a `group_name` under one top-level System settings submenu,
  invoked in ascending `order` (default `0`, ties keep registration order).

## [0.2.0]

- Game profiles can list up to 64 mid hook sites (was 16): the runtime exports `fefw_mid_0` .. `fefw_mid_63`.
- GitHub Actions updated to the Node 24 versions (checkout v7, setup-python v7, cache v6, upload-artifact v7).
- NRO loader: the bootstrap (PLT redirect and mid hook stubs, nn::ro loader) moved out of main's unused .text tail
  into its own module, `FEFW_RuntimePatcher/exefs/subsdk9`. The tail fit only about 20 mid hook sites; the module has
  no such limit and also has writable data. The loader maps subsdk9 right after main's `subsdk0..8`, at a fixed
  distance from main (`layout.subsdk9` in the game profile; saved profiles must be rebuilt), so main reaches it with
  plain `b` and the module needs no relocations. Eden does not layer new exefs files from `load/`, so deploy copies `subsdk9` into the game exefs.
- Mod texture files (`data`, `texture`, `icon_texture`, ...) are found in packaged mods too. When a package was read
  through its root `manifest.json` (the PC build), the runtime looked only in `<package>/resources/`, while packages
  ship them in `romfs/GameData/Classes/resources/`; the files were missed and the donor texture was kept (new
  portraits and icons showed the donor's picture). Lookup order now: the manifest's optional `resources_dir`
  (relative to the manifest), `resources/`, then `romfs/GameData/Classes/resources/`.
- MSBT patching: adding new labels now also extends the per-message TSY1 (style) and AOCM sections, so their entry
  counts match TXT2. A new entry takes its template label's values, otherwise the file's most common style.
  Previously these sections kept the old count, and other tools that validate MSBT files rejected the output.
- Plugin menu rows can take Left / Right: `menu_set_step(item, fn)` (appended to plugin API version 3) calls `fn`
  with -1 / +1; returning 1 marks the value as changed, as on the game's own setting rows.
- Plugin settings: `settings_get` / `settings_set` (appended to plugin API version 3) keep plugin options across
  boots. The Switch runtime stores them in the game's save data (`save:/fefw_settings.ini`).
- Plugin API version 3: `call_game(addr, args, nargs)` calls a game function. On the Switch it is a plain call; a host
  where game code does not run natively routes it through its own call mechanism, so plugins that call the game only
  through it can be built for both.
- Code-mod DLL `fefw_runtime_patcher.dll` for PC recompilation runtimes that load `mods/<name>/exefs/*.dll`
  (interface version 3+): the same data overlay, applied through the runtime's file edits. Native plugins are not
  loaded there.
- Mid-function hooks are generic: hook sites and their names come from the game profile, and plugins subscribe by
  name with `register_mid_hook(name, fn, user)` (plugin API version 2).
- The game title id is an input (`--title-id`, `FEFW_TITLE_ID`, or `title_id` in the hooks file / profile).

## [0.1.1]

First public release.

- Runtime data overlay for GameData tables, MSBT messages and icon archives, driven by mod manifests.
- NRO loader (default) and static `subsdk9` loader; native code plugins.
- Game-version-specific values come from a game profile, not from this repository.
- Build and deploy tool for Windows, Linux and macOS.
