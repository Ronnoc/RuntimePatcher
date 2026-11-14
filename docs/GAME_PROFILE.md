# Game profile

Everything that depends on one particular version of the game lives outside this repository, in a **game
profile**. The source code only names hooks and services (`fefw_hook_system_menu`, `FEFW_GAME_UI_ALLOC`, ...); the
profile says where they are.

A build gets its profile in one of two ways:

| Inputs | Typical use |
| --- | --- |
| `--game-exefs <dir>` + `--hooks <hooks.json>` (or `FEFW_GAME_EXEFS` + `FEFW_GAME_HOOKS`) | local build with your own game dump |
| `--profile <file or encoded text>` (or `FEFW_GAME_PROFILE`) | CI / any machine without game files |

## Hooks file

Hand-written JSON with the values that cannot be read from the game's symbol tables. Numbers may be JSON numbers
or `"0x..."` strings; keys starting with `_` and `note` fields are ignored.

```json
{
  "mid_hooks": [
    {"name": "fefw_hook_system_menu", "va": "0x1234560", "insn": "0x52800140", "note": "why this site"}
  ],
  "game": {
    "MENU_RESUME": "0x1234564",
    "MENU_RESUME_INSN": "0xA9454FF4",
    "UI_HEAP_GOT": "0x...",
    "UI_ALLOC": "0x...",
    "UI_SUBMENU": "0x...",
    "UI_ADD_ITEM": "0x...",
    "UI_REFRESH": "0x...",
    "UI_ITEM_VTABLE": "0x...",
    "UI_ITEM_VTABLE_BYTES": "0x...",
    "VT_TEXT": "0x...", "VT_CONFIRM": "0x...", "VT_RIGHT": "0x...", "VT_LEFT": "0x..."
  }
}
```

- `mid_hooks`: `name` is the runtime export that receives the hook (see `nx/src/nx_game_hooks.cpp`); `va` is the
  `main`-relative offset of one instruction, `insn` its original encoding. The build checks `insn` against your
  `main` and rejects PC-relative instructions. The order defines the hook index, so keep it stable.
- `game`: becomes `FEFW_GAME_<NAME>` macros in `build/nx/gen/fefw_game_offsets.h`, used by `nx/src/nx_game_hooks.cpp`.

Everything else (main's build id, PLT/GOT slots of the redirected imports, `malloc`, the sdk symbol offsets) is read
from the game files.

## Saving a profile for CI

A local build with `--game-exefs` + `--hooks` writes `build/nx/game_profile.json`. It also records which game
module exports every symbol the runtime and plugins import, so builds from the profile can still check linking.
Convert it to one line and store that as the repository secret `FEFW_GAME_PROFILE`:

```
python tools/game_profile.py encode build/nx/game_profile.json | gh secret set FEFW_GAME_PROFILE
```

Do this again after a game update, after changing the hooks file, or when a profile build fails with
"imports are not in the game profile".

Build logs never print profile addresses. The release files do contain them (the `.ips` patch writes to them), as
any binary patch for a game must.
