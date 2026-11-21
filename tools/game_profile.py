#!/usr/bin/env python3
"""Game profile: every game-build-specific value the runtime build needs, kept out of this repository.

A profile is JSON with:
  main        build id, .text size, rodata VA, GOT / PLT entries of the redirected imports, malloc
  sdk         MOD0 offset + offsets of the sdk exports the runtime calls before nn::ro binds it
  mid_hooks   [[export name, main offset, original instruction], ...]   (from the hooks file)
  game        {NAME: value}: main-relative addresses / sizes used by nx/src (FEFW_GAME_<NAME> macros)
  exports     {symbol: module} for the symbols the runtime / plugins import (link check without game files)

A local build with the game files (--game-exefs + --hooks <hand-written hooks file: {"mid_hooks", "game"}>) writes build/nx/game_profile.json. Turn it into one line for a CI secret (FEFW_GAME_PROFILE):

  python tools/game_profile.py encode build/nx/game_profile.json

The build reads a profile with --profile <file>, or FEFW_GAME_PROFILE (a path, or the encoded text).
"""
import argparse
import base64
import json
import os
import re
import struct
import sys
import zlib

import nso_tool

SCHEMA = 1
SDK_MODULES = ("main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "sdk")


def _hex(v):
    return int(v, 0) if isinstance(v, str) else int(v)


MAX_MID_HOOKS = 64  # number of fefw_mid_<k> entry points in the runtime


def load_hooks(path):
    with open(path, encoding="utf-8") as f:
        h = json.load(f)
    names = [e["name"] for e in h.get("mid_hooks", [])]
    bad = [n for n in names if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", n)]
    if bad or len(names) > MAX_MID_HOOKS:
        raise SystemExit(f"[!] {path}: mid hook names must be identifiers and at most {MAX_MID_HOOKS} hooks: {bad}")
    return {
        "title_id": h.get("title_id"),
        "mid_hooks": [[e["name"], _hex(e["va"]), _hex(e["insn"])] for e in h.get("mid_hooks", [])],
        "game": {k: _hex(v) for k, v in h.get("game", {}).items()},
    }


def all_exports(exefs):
    """{symbol: module} for every symbol the game's modules export (first module wins)."""
    out = {}
    for mod in SDK_MODULES:
        p = os.path.join(exefs, mod)
        if os.path.isfile(p):
            for name, defined, _v, _i in nso_tool.dynamic_symbols(nso_tool.load_nso_image(p)):
                if defined:
                    out.setdefault(name, mod)
    return out


def from_exefs(exefs, hooks, sdk_symbols):
    """Builds a profile (without `exports`) from the game exefs and a loaded hooks file."""
    import nro_bootstrap

    main = nro_bootstrap.analyze_main(os.path.join(exefs, "main"), hooks["mid_hooks"])
    img = nso_tool.load_nso_image(os.path.join(exefs, "sdk"))
    found = {n: v for n, d, v, _i in nso_tool.dynamic_symbols(img) if d}
    missing = [n for n in sdk_symbols if n not in found]
    if missing:
        raise SystemExit(f"[!] sdk does not export: {missing}")
    return {
        "schema": SCHEMA,
        "main": main,
        "layout": {"subsdk9": nro_bootstrap.subsdk9_offset(exefs)},
        "sdk": {"mod0": struct.unpack_from("<I", img, 4)[0], "syms": {n: found[n] for n in sdk_symbols}},
        "title_id": hooks["title_id"],
        "mid_hooks": hooks["mid_hooks"],
        "game": hooks["game"],
        "exports": {},
    }


def encode(profile):
    return base64.b64encode(zlib.compress(json.dumps(profile, separators=(",", ":")).encode(), 9)).decode()


def load(spec):
    """Loads a profile from a file path or from encode() output."""
    if os.path.isfile(spec):
        with open(spec, encoding="utf-8") as f:
            p = json.load(f)
    else:
        try:
            p = json.loads(zlib.decompress(base64.b64decode(spec.strip(), validate=True)))
        except Exception:
            raise SystemExit("[!] game profile is neither a file nor valid encoded profile text")
    if p.get("schema") != SCHEMA:
        raise SystemExit(f"[!] game profile schema {p.get('schema')} != {SCHEMA}; re-export it")
    return p


def save(profile, path):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(profile, f, indent=1, sort_keys=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("encode", help="print a profile as one line of text for a CI secret")
    c.add_argument("profile")
    args = ap.parse_args()
    print(encode(load(args.profile)))


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    main()
