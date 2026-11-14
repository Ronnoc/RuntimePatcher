# Changelog

All notable changes to this project are documented here. Versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

- Mid-function hooks are generic: hook sites and their names come from the game profile, and plugins subscribe by
  name with `register_mid_hook(name, fn, user)` (plugin API version 2).
- The game title id is an input (`--title-id`, `FEFW_TITLE_ID`, or `title_id` in the hooks file / profile).

## [0.1.1]

First public release.

- Runtime data overlay for GameData tables, MSBT messages and icon archives, driven by mod manifests.
- NRO loader (default) and static `subsdk9` loader; native code plugins.
- Game-version-specific values come from a game profile, not from this repository.
- Build and deploy tool for Windows, Linux and macOS.
