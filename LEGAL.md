# Legal

Fire Emblem, Fire Emblem: Fortune's Weave, Nintendo and Nintendo Switch 2 are trademarks of Nintendo. They are used
here only to say which game this software works with. This project is not affiliated with, sponsored by or endorsed
by Nintendo or Intelligent Systems.

## What this repository contains

Original code under the MIT licence (see [LICENSE](LICENSE)), one algorithm adapted from MIT-licensed code, and
build tooling that downloads third-party code under its own free licences (musl, libc++ headers, zstd). Details
and licence texts: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

It contains no game code, game data, decryption keys or firmware. Game-specific values the build needs (function
addresses and symbol offsets for one game version) are not stored in this repository: a build reads them from the
user's own game files or from a privately supplied game profile.

## Release downloads

Release archives contain this project's compiled code (with the libraries listed in THIRD_PARTY_NOTICES.md): a runtime module and a small patch for the game's
executable that redirects a few calls to it. To work, they necessarily include addresses inside one version of the
game; they include no game code, data or assets. They are useful only to someone who already has that game.

## Your responsibilities

- Use this software only with a copy of the game you own, dumped by you from your own console.
- Do not share, upload or attach game files, or anything extracted from them, here (including in issues, pull
  requests and mods published alongside this project).
- Mods made with this project are the responsibility of their authors.

The software is provided "as is", without warranty of any kind (see [LICENSE](LICENSE)). You use it at your own
risk; back up your saves.

## Contact

Rights holders can reach the maintainer by opening an issue titled "Rights holder contact" (no details needed in
public; we will reply with a private channel) or through GitHub's DMCA process.
