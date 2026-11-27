#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
FEFW RuntimePatcher — Emulator Build (runtime patcher + dependent class mods)
=================================================================================
One command builds everything an emulator (Eden / Citron) needs:

  release/emulator_ready/<title id>/
    FEFW_RuntimePatcher/
      exefs/<main build id>.ips      <- [nro, default] main patch: PLT redirects + mid hook branches into subsdk9
      exefs/subsdk9                  <- [nro, default] bootstrap module: hook stubs + nn::ro loader
      romfs/fefw/runtime_patcher.nro|nrr <- [nro, default] the runtime, loaded after boot
      exefs/subsdk9                  <- [static] the foundation as a static module (instead of the bootstrap)
    FEFW_<Mod>/romfs/GameData/Classes/...   <- every mods/*/manifest.json (data only, no IPS)

Usage
  python tools/build_emulator_mod.py [all|foundation|mods] [--deploy] [--loader nro|static]
                                     [--game-exefs DIR] [--clean]
                                     [--mods-dir DIR ...] [--out-dir DIR]
  --mods-dir: folders holding <id>/manifest.json (repeatable; default ./mods, or $FEFW_MODS_DIR, ';'/':'-separated)
  --out-dir:  root for release/emulator_ready/... (default: this repo, or $FEFW_OUT_DIR)
  nro (default): ships the bootstrap as exefs/subsdk9 in the add-on (Atmosphere loads it from contents/<title>/exefs);
                 deploy also copies it into the game exefs, since Eden started from a loose exefs folder does
                 not layer new exefs files. Nothing else in the game files or other mods is modified.
  static:        deploy installs subsdk9 into the game exefs;
  See docs/ADDING_A_NEW_CLASS.md for the class-authoring guide.

How it works
  * clang (--target=aarch64-none-elf) + header-only libc++ + musl headers (third_party/), no SDK.
  * nro: tools/nro_bootstrap.py builds nx/boot as the subsdk9 module and patches main's nn::fs PLT entries (and the
    mid hook sites) to branch into it; on the first romfs OpenFile it loads runtime_patcher.nro via nn::ro and forwards to its fefw_fs_*
    hooks. static: the module exports the nn::fs symbols main imports, so rtld binds main to it.
  * Either way Job.bin / Skill.bin / AssetTable.bin / msbt / icon archives are synthesized in memory.
  * Every undefined symbol of the module is checked against the game's real module exports.
"""

import argparse
import concurrent.futures
import glob
import json
import os
import re
import shutil
import struct
import subprocess
import sys

FOUNDATION_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS_DIR = os.path.join(FOUNDATION_DIR, "tools")
NX_DIR = os.path.join(FOUNDATION_DIR, "nx")
THIRD_PARTY = os.path.join(FOUNDATION_DIR, "third_party")
BUILD_DIR = os.path.join(FOUNDATION_DIR, "build", "nx")
SYSROOT = os.path.join(BUILD_DIR, "sysroot")
OBJ_DIR = os.path.join(BUILD_DIR, "obj")
# Game exefs (main, sdk, ... ): read for import validation + sdk offsets, and subsdk9 is installed here.
# Override with --game-exefs or the FEFW_GAME_EXEFS environment variable.
GAME_EXEFS = os.environ.get("FEFW_GAME_EXEFS", "")  # your extracted game exefs (main, sdk, ...)
# Game title id (16 hex digits): --title-id, $FEFW_TITLE_ID, or "title_id" in the hooks file / game profile.
TITLE_ID = None
with open(os.path.join(FOUNDATION_DIR, "VERSION"), encoding="utf-8") as _f:
    VERSION = _f.read().strip()
# Game-build-specific values (addresses, hooks): from --game-exefs + --hooks, or a saved --profile.
HOOKS_FILE = os.environ.get("FEFW_GAME_HOOKS")
PROFILE_SPEC = os.environ.get("FEFW_GAME_PROFILE")
PROFILE = None           # loaded game profile (tools/game_profile.py)
PROFILE_FROM_EXEFS = False
_ALL_EXPORTS = None      # every export of the game modules (only with the game exefs)
_ARGS = None             # parsed command line (profile options), for on-demand profile loading
RECORDED_IMPORTS = {}    # symbol -> module, for every import of the modules built in this run
MODS_DIRS = [d for d in os.environ.get("FEFW_MODS_DIR", "").split(os.pathsep) if d] or [os.path.join(FOUNDATION_DIR, "mods")]
OUT_ROOT = os.environ.get("FEFW_OUT_DIR") or FOUNDATION_DIR
RELEASE_DIR = None  # <out root>/release/emulator_ready/<title id>, set once the title id is known
FOUNDATION_PKG = "FEFW_RuntimePatcher"   # emulator add-on folder name
NRO_BASENAME = "runtime_patcher"         # romfs/fefw/runtime_patcher.nro + .nrr
MODULE_NAME = "subsdk9"
# Loader mode: "nro"    = runtime_patcher.nro loaded through nn::ro by a main bootstrap IPS (default; no game
#                         exefs changes, no rtld patch)
#              "static" = subsdk9 NSO in the game exefs (fallback)
LOADER = "nro"
NRO_ROMFS_DIR = "fefw"          # <pkg>/romfs/fefw/<NRO_BASENAME>.nro + .nrr

sys.path.insert(0, TOOLS_DIR)
import nso_tool
import nro_bootstrap  # noqa: E402
import game_profile  # noqa: E402

LLVM_BIN_CANDIDATES = [r"C:\Program Files\LLVM\bin", "/usr/bin", "/usr/local/bin"]


# ----------------------------------------------------------------------------------------------
# Toolchain
# ----------------------------------------------------------------------------------------------
def find_tool(name: str) -> str:
    exe = name + (".exe" if os.name == "nt" else "")
    found = shutil.which(exe)
    if found:
        return found
    for d in LLVM_BIN_CANDIDATES:
        p = os.path.join(d, exe)
        if os.path.isfile(p):
            return p
    raise SystemExit(f"[!] {name} not found (install LLVM: winget install LLVM.LLVM)")


def clang_resource_include(clang: str) -> str:
    out = subprocess.check_output([clang, "-print-resource-dir"], text=True).strip()
    return os.path.join(out, "include")


# ----------------------------------------------------------------------------------------------
# Sysroot: musl headers + libc++ __config_site
# ----------------------------------------------------------------------------------------------
CONFIG_SITE = """\
// Generated by build_emulator_mod.py — libc++ configuration for the FEFW subsdk9 module.
#ifndef _LIBCPP___CONFIG_SITE
#define _LIBCPP___CONFIG_SITE
#define _LIBCPP_ABI_VERSION 1
#define _LIBCPP_ABI_NAMESPACE __fefw
#define _LIBCPP_ABI_FORCE_ITANIUM 0
#define _LIBCPP_ABI_FORCE_MICROSOFT 0
#define _LIBCPP_HAS_THREADS 0
#define _LIBCPP_HAS_MONOTONIC_CLOCK 0
#define _LIBCPP_HAS_MUSL_LIBC 1
#define _LIBCPP_HAS_THREAD_API_PTHREAD 0
#define _LIBCPP_HAS_THREAD_API_EXTERNAL 0
#define _LIBCPP_HAS_THREAD_API_WIN32 0
#define _LIBCPP_HAS_THREAD_API_C11 0
#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 0
#define _LIBCPP_HAS_FILESYSTEM 0
#define _LIBCPP_HAS_RANDOM_DEVICE 0
#define _LIBCPP_HAS_LOCALIZATION 0
#define _LIBCPP_HAS_UNICODE 1
#define _LIBCPP_HAS_WIDE_CHARACTERS 0
#define _LIBCPP_HAS_TIME_ZONE_DATABASE 0
#define _LIBCPP_INSTRUMENTED_WITH_ASAN 0
#define _LIBCPP_PSTL_BACKEND_SERIAL
#define _LIBCPP_HARDENING_MODE_DEFAULT _LIBCPP_HARDENING_MODE_NONE
#define _LIBCPP_ASSERTION_SEMANTIC_DEFAULT _LIBCPP_ASSERTION_SEMANTIC_HARDENING_DEPENDENT
#define _LIBCPP_LIBC_PICOLIBC 0
#define _LIBCPP_LIBC_NEWLIB 0
#define _LIBCPP_LIBC_LLVM_LIBC 0
#endif
"""


def mkalltypes(arch_in: str, generic_in: str) -> str:
    out = []
    for path in (arch_in, generic_in):
        for line in open(path, encoding="utf-8").read().splitlines():
            m = re.match(r"^TYPEDEF (.*) ([^ ]*);$", line)
            if m:
                t, n = m.groups()
                out.append(f"#if defined(__NEED_{n}) && !defined(__DEFINED_{n})\ntypedef {t} {n};\n#define __DEFINED_{n}\n#endif\n")
                continue
            m = re.match(r"^(STRUCT|UNION) * ([^ ]*) (.*);$", line)
            if m:
                kw, n, body = m.groups()
                kw = kw.lower()
                out.append(f"#if defined(__NEED_{kw}_{n}) && !defined(__DEFINED_{kw}_{n})\n{kw} {n} {body};\n#define __DEFINED_{kw}_{n}\n#endif\n")
                continue
            out.append(line)
    return "\n".join(out) + "\n"


def ensure_sysroot():
    musl = os.path.join(THIRD_PARTY, "musl")
    libcxx = os.path.join(THIRD_PARTY, "llvm-project", "libcxx", "include")
    if not os.path.isdir(musl) or not os.path.isdir(libcxx):
        raise SystemExit("[!] third_party headers missing. Run: python tools/fetch_third_party.py")
    stamp = os.path.join(SYSROOT, ".stamp")
    if os.path.isfile(stamp):
        return
    if os.path.isdir(SYSROOT):
        shutil.rmtree(SYSROOT)
    inc = os.path.join(SYSROOT, "include")
    shutil.copytree(os.path.join(musl, "include"), inc, ignore=shutil.ignore_patterns("*.in"))
    bits = os.path.join(inc, "bits")
    os.makedirs(bits, exist_ok=True)
    for src in (os.path.join(musl, "arch", "generic", "bits"), os.path.join(musl, "arch", "aarch64", "bits")):
        for fn in os.listdir(src):
            if not fn.endswith(".in"):
                shutil.copy2(os.path.join(src, fn), os.path.join(bits, fn))
    with open(os.path.join(bits, "alltypes.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write(mkalltypes(os.path.join(musl, "arch", "aarch64", "bits", "alltypes.h.in"),
                           os.path.join(musl, "include", "alltypes.h.in")))
    sc_in = open(os.path.join(musl, "arch", "aarch64", "bits", "syscall.h.in"), encoding="utf-8").read()
    sc = sc_in + "".join(l.replace("__NR_", "SYS_") + "\n" for l in sc_in.splitlines() if "__NR_" in l)
    with open(os.path.join(bits, "syscall.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write(sc)

    cxxcfg = os.path.join(SYSROOT, "cxxcfg")
    os.makedirs(cxxcfg, exist_ok=True)
    with open(os.path.join(cxxcfg, "__config_site"), "w", encoding="utf-8", newline="\n") as f:
        f.write(CONFIG_SITE)
    shutil.copy2(os.path.join(THIRD_PARTY, "llvm-project", "libcxx", "vendor", "llvm", "default_assertion_handler.in"),
                 os.path.join(cxxcfg, "__assertion_handler"))
    open(stamp, "w").close()
    print(f"[+] sysroot generated: {SYSROOT}")


# ----------------------------------------------------------------------------------------------
# Compile & link
# ----------------------------------------------------------------------------------------------
ZSTD_URL = "https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz"
ZSTD_SHA256 = "eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3"  # same pin as CMakeLists.txt


def deps_cache_dir() -> str:
    """Download cache shared with the CMake build: $FEFW_DEPS_DIR, else the platform's user cache folder."""
    if os.environ.get("FEFW_DEPS_DIR"):
        return os.environ["FEFW_DEPS_DIR"]
    if os.name == "nt" and os.environ.get("LOCALAPPDATA"):
        base = os.environ["LOCALAPPDATA"]
    elif sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Caches")
    else:
        base = os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")
    return os.path.join(base, "fefw", "deps")


def zstd_lib_dir() -> str:
    lib = os.path.join(BUILD_DIR, "zstd-1.5.7", "lib")
    if os.path.isfile(os.path.join(lib, "zstd.h")):
        return lib
    import hashlib
    import tarfile
    import urllib.request
    tarball = os.path.join(deps_cache_dir(), "zstd-1.5.7.tar.gz")
    if not os.path.isfile(tarball):
        os.makedirs(os.path.dirname(tarball), exist_ok=True)
        print(f"[+] downloading {ZSTD_URL}")
        urllib.request.urlretrieve(ZSTD_URL, tarball + ".part")
        os.replace(tarball + ".part", tarball)
    with open(tarball, "rb") as f:
        if hashlib.sha256(f.read()).hexdigest() != ZSTD_SHA256:
            raise SystemExit(f"[!] {tarball}: SHA-256 mismatch; delete it and retry")
    os.makedirs(BUILD_DIR, exist_ok=True)
    with tarfile.open(tarball) as tf:
        tf.extractall(BUILD_DIR, filter="data")
    return lib


def collect_sources():
    zlib = zstd_lib_dir()
    cxx = [os.path.join(FOUNDATION_DIR, "src", f) for f in
           ("game_table.cpp", "msbt_patcher.cpp", "bntx_patcher.cpp", "vfs_overlay.cpp", "plugin_host_core.cpp")]
    cxx += sorted(glob.glob(os.path.join(NX_DIR, "src", "*.cpp")))
    c = []
    for sub in ("common", "compress", "decompress"):
        c += sorted(glob.glob(os.path.join(zlib, sub, "*.c")))
    asm = [os.path.join(NX_DIR, "crt0.S")]
    return cxx, c, asm, zlib


def common_flags(clang: str, zlib: str):
    return [
        "--target=aarch64-none-elf", "-march=armv8-a", "-mcpu=cortex-a57",
        "-fPIC", "-O2", "-g0", "-ffunction-sections", "-fdata-sections",
        "-fvisibility=hidden", "-fno-stack-protector", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
        "-fno-common", "-nostdinc", "-D__SWITCH__", "-DFEFW_NX_MODULE=1", f'-DFEFW_VERSION="{VERSION}"',
        "-DZSTD_DISABLE_ASM", "-DZSTD_NO_TRACE", "-DDEBUGLEVEL=0", "-DZSTD_LEGACY_SUPPORT=0",
        "-DZSTDLIB_VISIBILITY=", "-DZSTDERRORLIB_VISIBILITY=", "-DZSTDLIB_VISIBLE=", "-DZSTDERRORLIB_VISIBLE=",
        "-DZSTDLIB_HIDDEN=", "-DZSTDERRORLIB_HIDDEN=",
    ], [
        "-isystem", clang_resource_include(clang),
        "-isystem", os.path.join(SYSROOT, "include"),
        "-I", zlib,
    ]


def compile_all(clang: str, jobs: int):
    cxx, c, asm, zlib = collect_sources()
    flags, sys_inc = common_flags(clang, zlib)
    cxx_flags = flags + [
        "-std=c++20", "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-fvisibility-inlines-hidden",
        "-nostdinc++", "-D_LIBCPP_DISABLE_EXTERN_TEMPLATE", "-D_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER_DISABLED",
        "-DFEFW_FOUNDATION_NO_STD_FILESYSTEM=1", "-Wno-unused-command-line-argument",
        "-isystem", os.path.join(SYSROOT, "cxxcfg"),
        "-isystem", os.path.join(THIRD_PARTY, "llvm-project", "libcxx", "include"),
    ] + sys_inc + ["-I", os.path.join(FOUNDATION_DIR, "include"), "-I", os.path.join(NX_DIR, "src"), "-I", os.path.join(BUILD_DIR, "gen")]
    if LOADER == "nro":
        cxx_flags += ["-DFEFW_NX_NRO=1"]
        flags = flags + ["-DFEFW_NX_NRO=1"]
    c_flags = flags + ["-std=c11", "-w"] + sys_inc
    obj_dir = OBJ_DIR if LOADER == "static" else OBJ_DIR + "_nro"
    os.makedirs(obj_dir, exist_ok=True)
    tasks = []
    for src in cxx:
        tasks.append((src, cxx_flags))
    for src in c:
        tasks.append((src, c_flags))
    for src in asm:
        tasks.append((src, flags))

    def obj_name(src):
        rel = os.path.relpath(src, os.path.dirname(FOUNDATION_DIR)) if src.startswith(FOUNDATION_DIR) else src
        return os.path.join(obj_dir, re.sub(r"[\\/:. ]+", "_", rel) + ".o")

    def run(task):
        src, fl = task
        obj = obj_name(src)
        if os.path.isfile(obj) and os.path.getmtime(obj) >= os.path.getmtime(src) and not src.endswith((".cpp", ".h")):
            return obj, None  # C/asm third-party objects are cached; C++ always rebuilt (header deps)
        cmd = [clang] + fl + ["-c", src, "-o", obj]
        p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
        if p.returncode != 0:
            return obj, f"{os.path.basename(src)}:\n{p.stderr}"
        return obj, None

    objs, errors = [], []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for obj, err in ex.map(run, tasks):
            objs.append(obj)
            if err:
                errors.append(err)
    if errors:
        for e in errors:
            print(e[:6000])
        raise SystemExit(f"[!] {len(errors)} compile error(s)")
    print(f"[+] compiled {len(objs)} objects")
    return objs


def link(lld: str, objs, out_elf: str):
    vscript = os.path.join(BUILD_DIR, "exports.map")
    if LOADER == "nro":
        # every entry point the main bootstrap looks up by name (PLT redirects + mid-function hooks)
        exports = ([exp for _imp, exp in nro_bootstrap.PLT_HOOKS] +
                   nro_bootstrap.mid_hook_exports(PROFILE["mid_hooks"]) + ["fefw_nx_module_init"])
    else:
        exports = ["_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", "_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm",
                   "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE",
                   "_ZN2nn2fs9CloseFileENS0_10FileHandleE", "fefw_nx_module_init"]
    with open(vscript, "w", encoding="utf-8") as f:
        f.write("{\n  global:\n" + "".join(f"    {e};\n" for e in exports) + "  local: *;\n};\n")
    rsp = os.path.join(BUILD_DIR, "link.rsp")
    with open(rsp, "w", encoding="utf-8") as f:
        f.write("\n".join('"' + o.replace("\\", "/") + '"' for o in objs))
    cmd = [lld, "-shared", "--gc-sections", "-T", os.path.join(NX_DIR, "link.ld"),
           "--version-script", vscript, "--hash-style=sysv", "-z", "now", "-z", "noexecstack",
           "-z", "max-page-size=0x1000", "-Bsymbolic", "--init=fefw_nx_module_init",
           "--no-undefined-version", "--allow-shlib-undefined",
           "-soname", MODULE_NAME if LOADER == "static" else "fefw_foundation",
           "-o", out_elf, "@" + rsp]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout + p.stderr)
        raise SystemExit("[!] link failed")
    print(f"[+] linked {out_elf} ({os.path.getsize(out_elf):,} bytes)")


# ----------------------------------------------------------------------------------------------
# Validation: every import must be exported by a real game module
# ----------------------------------------------------------------------------------------------
def elf_dynsyms(path: str):
    elf = open(path, "rb").read()
    shoff, = struct.unpack_from("<Q", elf, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x3A)
    secs = [struct.unpack_from("<IIQQQQIIQQ", elf, shoff + i * shentsize) for i in range(shnum)]
    dynsym = next(s for s in secs if s[1] == 11)
    strtab = secs[dynsym[6]]
    out = []
    for k in range(1, dynsym[5] // 24):
        name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", elf, dynsym[4] + k * 24)
        end = elf.index(b"\0", strtab[4] + name)
        out.append((elf[strtab[4] + name:end].decode("latin1"), shndx != 0, info >> 4))
    return out


def validate_imports(out_elf: str):
    global _ALL_EXPORTS
    load_profile(_ARGS)
    if PROFILE_FROM_EXEFS:
        if _ALL_EXPORTS is None:
            _ALL_EXPORTS = game_profile.all_exports(GAME_EXEFS)
        exports = _ALL_EXPORTS
    else:
        exports = PROFILE["exports"]
    syms = elf_dynsyms(out_elf)
    missing, used = [], {}
    for name, defined, bind in syms:
        if defined:
            continue
        if name in exports:
            used.setdefault(exports[name], []).append(name)
            RECORDED_IMPORTS[name] = exports[name]
        elif bind != 2:  # STB_WEAK may stay unresolved
            missing.append(name)
    exported = sorted(n for n, d, _ in syms if d)
    print(f"[+] module exports: {', '.join(exported)}")
    for mod, names in sorted(used.items()):
        print(f"    imports from {mod}: {len(names)} ({', '.join(sorted(names)[:12])}{' ...' if len(names) > 12 else ''})")
    if missing:
        print("[!] unresolved imports (not exported by any game module):")
        for n in sorted(missing):
            print("     ", n)
        if not PROFILE_FROM_EXEFS:
            raise SystemExit("[!] these imports are not in the game profile: rebuild locally with the game files "
                             "and update the profile (tools/game_profile.py)")
        raise SystemExit("[!] module would fail to link at boot")
    need = {"_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", "_ZN2nn2fs9CloseFileENS0_10FileHandleE"}
    if LOADER == "static" and not need.issubset(exported):
        raise SystemExit("[!] nn::fs interposer symbols are not exported")


# ----------------------------------------------------------------------------------------------
# Packaging
# ----------------------------------------------------------------------------------------------
# ----------------------------------------------------------------------------------------------
# Static fallback: offsets of the real nn::fs entry points inside the game's sdk module.
# ----------------------------------------------------------------------------------------------
SDK_FS_SYMBOLS = [
    "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",
    "_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm",
    "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm",
    "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE",
    "_ZN2nn2fs9CloseFileENS0_10FileHandleE",
    "_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc",
    "_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci",
    "_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE",
    "_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl",
    "_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE",
    # nn::ro (NRO loading; used by the NRO probe / future dynamic foundation loader)
    "_ZN2nn2ro10InitializeEv",
    "_ZN2nn2ro13GetBufferSizeEPmPKv",
    "_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv",
    "_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi",
    "_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc",
]
GEN_DIR = os.path.join(BUILD_DIR, "gen")


def generate_headers():
    """Writes build/nx/gen/fefw_sdk_offsets.h and fefw_game_offsets.h from the game profile."""
    sdk = PROFILE["sdk"]
    missing = [n for n in SDK_FS_SYMBOLS if n not in sdk["syms"]]
    if missing:
        raise SystemExit(f"[!] game profile lacks sdk symbols: {missing}")
    os.makedirs(GEN_DIR, exist_ok=True)
    with open(os.path.join(GEN_DIR, "fefw_sdk_offsets.h"), "w", encoding="utf-8") as f:
        f.write("// Generated from the game profile. Do not edit.\n#pragma once\n")
        f.write(f"#define FEFW_SDK_MOD0_OFFSET 0x{sdk['mod0']:x}u\n")
        f.write("struct FefwSdkSym { const char* name; unsigned long long offset; };\n")
        f.write("static const FefwSdkSym kFefwSdkSyms[] = {\n")
        for n in SDK_FS_SYMBOLS:
            f.write(f'    {{"{n}", 0x{sdk["syms"][n]:x}ull}},\n')
        f.write("};\n")
    with open(os.path.join(GEN_DIR, "fefw_game_offsets.h"), "w", encoding="utf-8") as f:
        f.write("// Generated from the game profile. Do not edit.\n#pragma once\n")
        for k, v in sorted(PROFILE["game"].items()):
            f.write(f"#define FEFW_GAME_{k} 0x{v:x}ull\n")
        names = "".join(f'"{name}", ' for name, _va, _insn in PROFILE["mid_hooks"])
        f.write(f"#define FEFW_MAX_MID_HOOKS {nro_bootstrap.MAX_MID_HOOKS}\n")
        f.write(f'#define FEFW_MID_HOOK_NAMES {names}""\n')


def load_profile(args):
    """Resolves the game profile: --profile / FEFW_GAME_PROFILE, else game exefs + hooks file."""
    global PROFILE, PROFILE_FROM_EXEFS
    if PROFILE is not None:
        return
    spec = args.profile or PROFILE_SPEC
    hooks = args.hooks or HOOKS_FILE
    if spec:
        PROFILE = game_profile.load(spec)
        print(f"[+] game profile loaded (main {PROFILE['main']['build_id'][:16]}...)")
    elif hooks and os.path.isfile(os.path.join(GAME_EXEFS, "sdk")):
        sdk_syms = SDK_FS_SYMBOLS + [n for n in nro_bootstrap.SDK_SYMS.values() if n not in SDK_FS_SYMBOLS]
        PROFILE = game_profile.from_exefs(GAME_EXEFS, game_profile.load_hooks(hooks), sdk_syms)
        PROFILE_FROM_EXEFS = True
        print(f"[+] game profile read from the game exefs + {os.path.basename(hooks)}")
    else:
        raise SystemExit("[!] no game profile: pass --profile <file> (or FEFW_GAME_PROFILE), or --game-exefs <dir> "
                         "with --hooks <file> (or FEFW_GAME_HOOKS)")


def resolve_title_id(args) -> str:
    tid = args.title_id or os.environ.get("FEFW_TITLE_ID")
    if not tid:
        if PROFILE is None and (args.profile or PROFILE_SPEC or args.hooks or HOOKS_FILE):
            load_profile(args)
        tid = (PROFILE or {}).get("title_id")
    if not tid or not re.fullmatch(r"[0-9A-Fa-f]{16}", tid):
        raise SystemExit("[!] game title id unknown or invalid: pass --title-id, set FEFW_TITLE_ID, or add "
                         '"title_id" to the hooks file')
    return tid.upper()


def save_recorded_profile():
    if PROFILE_FROM_EXEFS:
        PROFILE["exports"] = dict(sorted(RECORDED_IMPORTS.items()))
        out = os.path.join(BUILD_DIR, "game_profile.json")
        game_profile.save(PROFILE, out)
        print(f"[+] game profile saved: {out}\n    (one line for CI: python tools/game_profile.py encode <that file>)")


def build_foundation(jobs: int):
    clang = find_tool("clang")
    lld = find_tool("ld.lld")
    ensure_sysroot()
    generate_headers()
    objs = compile_all(clang, jobs)
    out_elf = os.path.join(BUILD_DIR, (MODULE_NAME if LOADER == "static" else NRO_BASENAME) + ".elf")
    link(lld, objs, out_elf)
    validate_imports(out_elf)
    pkg = os.path.join(RELEASE_DIR, FOUNDATION_PKG)
    if os.path.isdir(pkg):
        shutil.rmtree(pkg)
    if LOADER == "nro":
        dst = os.path.join(pkg, "romfs", NRO_ROMFS_DIR)
        nro = os.path.join(dst, NRO_BASENAME + ".nro")
        nso_tool.cmd_elf2nro(argparse.Namespace(elf=out_elf, out=nro))
        nso_tool.cmd_mknrr(argparse.Namespace(out=os.path.join(dst, NRO_BASENAME + ".nrr"), nro=[nro],
                                              program_id=TITLE_ID, format=1))
        build_id, ips, _records, boot_nso = nro_bootstrap.build(NX_DIR, os.path.join(BUILD_DIR, "boot"), PROFILE,
                                                     find_tool("clang"), find_tool("ld.lld"))
        os.makedirs(os.path.join(pkg, "exefs"), exist_ok=True)
        with open(os.path.join(pkg, "exefs", build_id + ".ips"), "wb") as f:
            f.write(ips)
        shutil.copy2(boot_nso, os.path.join(pkg, "exefs", MODULE_NAME))
        with open(os.path.join(pkg, "README.txt"), "w", encoding="utf-8") as f:
            f.write(f"FEFW RuntimePatcher {VERSION} (NRO loader)\n"
                    "exefs/<main build id>.ips redirects main's nn::fs PLT entries and hook sites to the bootstrap in\n"
                    "exefs/subsdk9, which loads romfs/fefw/runtime_patcher.nro through nn::ro. No rtld patch required.\n"
                    "Required by every FEFW class mod; enable it in the emulator's add-on list.\n")
        return nro
    out_nso = os.path.join(pkg, "exefs", MODULE_NAME)
    nso_tool.cmd_elf2nso(argparse.Namespace(elf=out_elf, out=out_nso))
    with open(os.path.join(pkg, "README.txt"), "w", encoding="utf-8") as f:
        f.write(f"FEFW RuntimePatcher {VERSION} (static subsdk9 loader)\n"
                "Required by every FEFW class mod. Copy this folder next to the class mod folders in\n"
                f"<emulator>/load/{TITLE_ID}/ and enable it in the emulator's add-on list.\n")
    return out_nso


def discover_mods():
    mods = []
    manifests = sorted(mp for d in MODS_DIRS for mp in glob.glob(os.path.join(d, "*", "manifest.json")))
    if not manifests:
        print(f"[i] no mods found in {os.pathsep.join(MODS_DIRS)}")
    for mp in manifests:
        with open(mp, encoding="utf-8") as f:
            m = json.load(f)
        if not m.get("enabled", True):
            continue
        m["_mod_dir"] = os.path.dirname(mp)
        mods.append(m)
    return mods


def package_name(mod_id: str) -> str:
    return "FEFW_" + "_".join(w.capitalize() for w in re.split(r"[_\-\s]+", mod_id) if w) + "_Mod"


PLUGIN_ROMFS_DIR = "fefw/plugins"   # <pkg>/romfs/fefw/plugins/<name>.nro + .nrr, loaded by the runtime


def build_plugin(m, pkg):
    """Builds a mod's code plugin (manifest "plugin": {"name", "sources"}) into an NRO + NRR."""
    spec = m["plugin"]
    name = spec["name"]
    clang = find_tool("clang")
    lld = find_tool("ld.lld")
    out_dir = os.path.join(BUILD_DIR, "plugins", name)
    os.makedirs(out_dir, exist_ok=True)
    base = ["--target=aarch64-none-elf", "-march=armv8-a", "-mcpu=cortex-a57", "-fPIC", "-O2", "-g0",
            "-ffunction-sections", "-fdata-sections", "-fvisibility=hidden", "-fno-stack-protector",
            "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-ffreestanding", "-nostdinc",
            "-DFEFW_NX_NRO=1", "-isystem", clang_resource_include(clang),
            "-I", os.path.join(FOUNDATION_DIR, "include")]
    cxx = ["-std=c++20", "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-nostdinc++"]
    objs = []
    for src in [os.path.join(m["_mod_dir"], s) for s in spec["sources"]] + [os.path.join(NX_DIR, "crt0.S")]:
        obj = os.path.join(out_dir, re.sub(r"[\\/:. ]+", "_", os.path.basename(src)) + ".o")
        fl = base + (cxx if src.endswith(".cpp") else [])
        p = subprocess.run([clang] + fl + ["-c", src, "-o", obj], capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        if p.returncode != 0:
            raise SystemExit(f"[!] plugin {name}: compile failed ({os.path.basename(src)}):\n{p.stderr[:6000]}")
        objs.append(obj)
    vscript = os.path.join(out_dir, "exports.map")
    with open(vscript, "w", encoding="utf-8") as f:
        f.write("{ global: fefw_plugin_main; local: *; };\n")
    elf = os.path.join(out_dir, name + ".elf")
    p = subprocess.run([lld, "-shared", "--gc-sections", "-T", os.path.join(NX_DIR, "link.ld"),
                        "--version-script", vscript, "--hash-style=sysv", "-z", "now", "-z", "noexecstack",
                        "-z", "max-page-size=0x1000", "-Bsymbolic", "--allow-shlib-undefined",
                        "-soname", "fefw_plugin_" + name, "-o", elf] + objs, capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit(f"[!] plugin {name}: link failed:\n{p.stdout}{p.stderr}")
    validate_imports(elf)
    dst = os.path.join(pkg, "romfs", *PLUGIN_ROMFS_DIR.split("/"))
    nro = os.path.join(dst, name + ".nro")
    nso_tool.cmd_elf2nro(argparse.Namespace(elf=elf, out=nro))
    nso_tool.cmd_mknrr(argparse.Namespace(out=os.path.join(dst, name + ".nrr"), nro=[nro], program_id=TITLE_ID,
                                          format=1))


def _write_overlay_manifest(m, classes_dir):
    """Write a single plain-text JSON manifest into romfs/GameData/Classes/<id>.json.

    Both PC and Switch/emulator read this exact file, so players can edit class stats on either platform
    and startup never parses a duplicate root manifest.json. Large machine-generated data-only manifests
    (> 256 KiB without `classes`) are written compactly to reduce disk size and startup parse time.
    """
    clean = {k: v for k, v in m.items() if not k.startswith("_")}
    pretty = json.dumps(clean, ensure_ascii=False, indent=2)
    if not clean.get("classes") and len(pretty.encode("utf-8")) > 256 * 1024:
        text = json.dumps(clean, ensure_ascii=False, separators=(",", ":"))
    else:
        text = pretty + "\n"
    out_path = os.path.join(classes_dir, f"{m['id']}.json")
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def build_mods():
    built = []
    for m in discover_mods():
        pkg_name = package_name(m["id"])
        pkg = os.path.join(RELEASE_DIR, pkg_name)
        if os.path.isdir(pkg):
            shutil.rmtree(pkg)
        os.makedirs(pkg, exist_ok=True)
        classes_dir = os.path.join(pkg, "romfs", "GameData", "Classes")
        # Both PC and Switch discover mod manifests through romfs/GameData/Classes/<id>.json.
        if m.get("classes") or m.get("table_patches") or m.get("msbt_patches") or m.get("bntx_patches"):
            os.makedirs(classes_dir, exist_ok=True)
            _write_overlay_manifest(m, classes_dir)
        if m.get("plugin"):
            build_plugin(m, pkg)
        # Plain romfs files shipped by the mod (new models, ...): mods/<id>/romfs/** -> <pkg>/romfs/**
        own_romfs = os.path.join(m["_mod_dir"], "romfs")
        if os.path.isdir(own_romfs):
            shutil.copytree(own_romfs, os.path.join(pkg, "romfs"), dirs_exist_ok=True)
        res_src = os.path.join(m["_mod_dir"], "resources")
        if os.path.isdir(res_src) and os.path.isdir(classes_dir):  # resources/*.swizzled (icons, faces, ...)
            for fn in os.listdir(res_src):
                if fn.endswith(".swizzled"):
                    os.makedirs(os.path.join(classes_dir, "resources"), exist_ok=True)
                    shutil.copy2(os.path.join(res_src, fn), os.path.join(classes_dir, "resources", fn))
        files = sorted(os.path.relpath(p, pkg) for p in glob.glob(os.path.join(pkg, "**", "*"), recursive=True)
                       if os.path.isfile(p))
        print(f"[+] {pkg_name}: {len(files)} file(s), romfs only (requires {FOUNDATION_PKG})")
        built.append(pkg_name)
    return built


def install_into_game_exefs(nso_path: str):
    """Eden only lets add-ons *replace* NSOs the title already has; the base game has no subsdk9, so
    load/<title>/FEFW_RuntimePatcher/exefs/subsdk9 is silently ignored. Install it next to main."""
    if not GAME_EXEFS or not os.path.isdir(GAME_EXEFS):
        print(f"    [!] game exefs not found at {GAME_EXEFS}; copy subsdk9 there manually")
        return
    dst = os.path.join(GAME_EXEFS, MODULE_NAME)
    if os.path.isfile(dst) and not is_our_module(dst):
        print(f"    [!] the game exefs already has a {MODULE_NAME} that is not ours; not overwriting it")
        return
    shutil.copy2(nso_path, os.path.join(GAME_EXEFS, MODULE_NAME))
    print(f"[+] installed {MODULE_NAME} into game exefs: {GAME_EXEFS}")


def check_main_ips_overlap(load_dir: str):
    ours = glob.glob(os.path.join(load_dir, FOUNDATION_PKG, "exefs", "*.ips"))
    if not ours:
        return
    mine = nro_bootstrap.ips_ranges(ours[0])
    name = os.path.basename(ours[0])
    for other in glob.glob(os.path.join(load_dir, "*", "exefs", name)):
        if os.path.dirname(os.path.dirname(other)) == os.path.join(load_dir, FOUNDATION_PKG):
            continue
        hits = [(a, b) for a, b in nro_bootstrap.ips_ranges(other) for c, d in mine if a < d and c < b]
        mod = os.path.basename(os.path.dirname(os.path.dirname(other)))
        if hits:
            print(f"    [!] main IPS of '{mod}' overlaps ours at {[hex(a) for a, _ in hits[:5]]}")
        else:
            print(f"    [=] no overlap with main IPS of '{mod}'")


LOADER_MARKER = ".fefw_loader"  # in each emulator load/<title>/: which loader mode was deployed last


def note_loader_switch(load_dir: str):
    marker = os.path.join(load_dir, LOADER_MARKER)
    prev = open(marker, encoding="utf-8").read().strip() if os.path.isfile(marker) else None
    if prev and prev != LOADER:
        print(f"    [!] loader switched {prev} -> {LOADER} in {load_dir} "
              f"(another build may have deployed {prev}; deploy from one place only)")
    with open(marker, "w", encoding="utf-8") as f:
        f.write(LOADER + "\n")


def is_our_module(path: str) -> bool:
    """True only for a subsdk9 built by this tool (it exports fefw_nx_module_init)."""
    try:
        return any(d and n == "fefw_nx_module_init"
                   for n, d, _v, _i in nso_tool.dynamic_symbols(nso_tool.load_nso_image(path)))
    except Exception:
        return False


EMULATORS = ("eden", "citron", "yuzu", "suyu", "sudachi")


def emulator_dirs():
    """Emulator user folders to deploy into: $FEFW_EMU_DIR (path list), else every installed emulator's data
    folder (%APPDATA% on Windows, ~/Library/Application Support on macOS, $XDG_DATA_HOME or ~/.local/share)."""
    if os.environ.get("FEFW_EMU_DIR"):
        return [d for d in os.environ["FEFW_EMU_DIR"].split(os.pathsep) if d]
    if os.name == "nt":
        base = os.environ.get("APPDATA", "")
    elif sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Application Support")
    else:
        base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    dirs = [os.path.join(base, e) for e in EMULATORS if base and os.path.isdir(os.path.join(base, e))]
    if not dirs:
        print("    [!] no emulator user folder found; set FEFW_EMU_DIR to e.g. <path>/eden")
    return dirs


def deploy(packages):
    # Only files this tool created are ever replaced or removed; anything else is reported and left alone.
    if FOUNDATION_PKG in packages and LOADER == "static":
        install_into_game_exefs(os.path.join(RELEASE_DIR, FOUNDATION_PKG, "exefs", MODULE_NAME))
    elif FOUNDATION_PKG in packages and GAME_EXEFS:
        # Eden started from a loose exefs folder does not layer new exefs files from load/, so the bootstrap
        # module goes next to main (replacing only a static subsdk9 or bootstrap of ours).
        dst = os.path.join(GAME_EXEFS, MODULE_NAME)
        if os.path.isfile(dst) and not (is_our_module(dst) or nro_bootstrap._is_boot_module(dst)):
            print(f"    [!] the game exefs has a {MODULE_NAME} that is not ours; left untouched")
        elif os.path.isdir(GAME_EXEFS):
            shutil.copy2(os.path.join(RELEASE_DIR, FOUNDATION_PKG, "exefs", MODULE_NAME), dst)
            print(f"[+] NRO loader: installed the bootstrap {MODULE_NAME} into the game exefs: {GAME_EXEFS}")
    for emu_dir in emulator_dirs():
        emu = os.path.basename(emu_dir)
        load = os.path.join(emu_dir, "load", TITLE_ID)
        os.makedirs(load, exist_ok=True)
        for name in packages:
            dst = os.path.join(load, name)
            if os.path.isdir(dst):
                shutil.rmtree(dst)
            shutil.copytree(os.path.join(RELEASE_DIR, name), dst)
        print(f"[+] deployed {len(packages)} package(s) to {load}")
        if FOUNDATION_PKG in packages:
            note_loader_switch(load)
        if LOADER != "static":
            check_main_ips_overlap(load)


PROBE_PKG = "FEFW_NRO_Probe"


def build_nro_probe():
    """Diagnostic package: a tiny NRO + NRR that subsdk9 loads through nn::ro (see nx/src/nx_nro_probe.cpp)."""
    clang = find_tool("clang")
    lld = find_tool("ld.lld")
    probe_dir = os.path.join(NX_DIR, "probe")
    out_dir = os.path.join(BUILD_DIR, "probe")
    os.makedirs(out_dir, exist_ok=True)
    flags = ["--target=aarch64-none-elf", "-march=armv8-a", "-fPIC", "-O2", "-g0", "-fvisibility=hidden",
             "-ffreestanding", "-fno-exceptions", "-fno-rtti", "-fno-stack-protector",
             "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-nostdinc", "-nostdinc++"]
    objs = []
    for src in ("nro_crt0.S", "probe.cpp"):
        obj = os.path.join(out_dir, src + ".o")
        cmd = [clang] + flags + ["-isystem", clang_resource_include(clang), "-c", os.path.join(probe_dir, src), "-o", obj]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode != 0:
            raise SystemExit(f"[!] probe compile failed: {p.stderr}")
        objs.append(obj)
    vscript = os.path.join(out_dir, "exports.map")
    with open(vscript, "w", encoding="utf-8") as f:
        f.write("{ global: fefw_probe_hello; local: *; };")
    elf = os.path.join(out_dir, "probe.elf")
    cmd = [lld, "-shared", "--gc-sections", "-T", os.path.join(NX_DIR, "link.ld"), "--version-script", vscript,
           "--hash-style=sysv", "-z", "now", "-z", "max-page-size=0x1000", "-Bsymbolic", "-soname", "fefw_probe",
           "-o", elf] + objs
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit(f"[!] probe link failed: {p.stdout}{p.stderr}")
    pkg = os.path.join(RELEASE_DIR, PROBE_PKG)
    if os.path.isdir(pkg):
        shutil.rmtree(pkg)
    dst = os.path.join(pkg, "romfs", "fefw_probe")
    nro = os.path.join(dst, "probe.nro")
    nso_tool.cmd_elf2nro(argparse.Namespace(elf=elf, out=nro))
    nso_tool.cmd_mknrr(argparse.Namespace(out=os.path.join(dst, "probe.nrr"), nro=[nro], program_id=TITLE_ID, format=1))
    print(f"[+] {PROBE_PKG}: romfs/fefw_probe/probe.nro + probe.nrr (diagnostic; remove after testing)")
    return PROBE_PKG


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("target", nargs="?", default="all", choices=["all", "foundation", "mods", "probe"])
    ap.add_argument("--deploy", "-d", action="store_true", help="copy packages into the emulator load/ directories")
    ap.add_argument("--game-exefs", help="your extracted game exefs directory (default: $FEFW_GAME_EXEFS)")
    ap.add_argument("--loader", choices=["nro", "static"], default="nro",
                    help="nro (default): runtime_patcher.nro via nn::ro + main bootstrap IPS; "
                         "static: subsdk9 in game exefs (fallback)")
    ap.add_argument("--mods-dir", action="append", help="mod source folder (repeatable; default ./mods)")
    ap.add_argument("--out-dir", help="root for release/emulator_ready/ (default: this repo)")
    ap.add_argument("--hooks", help="game hooks file used with --game-exefs (default: $FEFW_GAME_HOOKS)")
    ap.add_argument("--profile", help="saved game profile, file or encoded text (default: $FEFW_GAME_PROFILE); "
                                      "replaces --game-exefs/--hooks")
    ap.add_argument("--title-id", help="game title id, 16 hex digits (default: $FEFW_TITLE_ID, else the game "
                                       "profile / hooks file)")
    ap.add_argument("--clean", action="store_true", help="remove build/nx before building")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    global GAME_EXEFS, LOADER, MODS_DIRS, RELEASE_DIR, TITLE_ID, _ARGS
    _ARGS = args
    if args.mods_dir:
        MODS_DIRS = [os.path.abspath(d) for d in args.mods_dir]
    LOADER = "static" if args.target == "probe" else args.loader  # the NRO probe runs inside static subsdk9
    if args.game_exefs:
        GAME_EXEFS = os.path.abspath(args.game_exefs)
    if args.target in ("all", "foundation", "probe") or args.deploy:
        load_profile(args)
    TITLE_ID = resolve_title_id(args)
    RELEASE_DIR = os.path.join(os.path.abspath(args.out_dir) if args.out_dir else OUT_ROOT,
                               "release", "emulator_ready", TITLE_ID)

    if args.clean and os.path.isdir(BUILD_DIR):
        shutil.rmtree(BUILD_DIR)
    os.makedirs(RELEASE_DIR, exist_ok=True)

    packages = []
    if args.target in ("all", "foundation"):
        build_foundation(args.jobs)
        packages.append(FOUNDATION_PKG)
    if args.target in ("all", "mods"):
        packages += build_mods()
    if args.target == "probe":
        build_foundation(args.jobs)
        packages.append(FOUNDATION_PKG)
        packages.append(build_nro_probe())
    save_recorded_profile()
    if args.deploy:
        deploy(packages)
    print(f"\n[✓] FEFW RuntimePatcher {VERSION}: emulator-ready output: {RELEASE_DIR}")


if __name__ == "__main__":
    main()
