"""FEFW NRO bootstrap builder (experimental `--loader nro`).

Produces an IPS32 patch for the game's `main` that
  * places nx/boot (dispatcher + freestanding C loader) in main's unused .text tail, and
  * rewrites the PLT entries of nn::fs::{OpenFile, ReadFile, ReadFile(size_t*), GetFileSize, CloseFile}
    to `b fefw_boot_stub_N`.
The first OpenFile after the romfs is mounted loads romfs:/fefw/runtime_patcher.nro through nn::ro; from then on
the stubs forward to the NRO's fefw_fs_* hooks (looked up statelessly with nn::ro::LookupSymbol).

Besides nn::fs it also redirects nn::hid::GetNpadState (per-frame tick + input injection for plugins) and places
mid-function hooks (game profile `mid_hooks`: one instruction in main replaced by `b fefw_boot_mid_N`, which calls the NRO
export with a register context, then runs the displaced instruction and resumes).
"""
import os
import struct
import subprocess

import nso_tool

FS_IMPORTS = [
    "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",
    "_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm",
    "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm",
    "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE",
    "_ZN2nn2fs9CloseFileENS0_10FileHandleE",
]
# (main import, NRO export). Order == hook index == stub index == fefw_got_N. OpenFile must stay index 0
# (boot.c loads the NRO from it).
PLT_HOOKS = list(zip(FS_IMPORTS, ["fefw_fs_OpenFile", "fefw_fs_ReadFile", "fefw_fs_ReadFileEx",
                                  "fefw_fs_GetFileSize", "fefw_fs_CloseFile"])) + [
    ("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", "fefw_hid_GetNpadStateHandheld"),
    ("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", "fefw_hid_GetNpadStateJoyDual"),
    ("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", "fefw_hid_GetNpadStateFullKey"),
]
PLT_IMPORTS = [imp for imp, _ in PLT_HOOKS]
# Mid-function hooks come from the game profile (tools/game_profile.py): [name, main offset, original
# instruction]. Hook index = len(PLT_HOOKS) + position; mid hook k enters the runtime through export fefw_mid_<k>.
MAX_MID_HOOKS = 64  # = FEFW_MAX_MID_HOOKS (number of fefw_mid_<k> exports in nx/src/nx_game_hooks.cpp)


def mid_hook_exports(mid_hooks):
    return [f"fefw_mid_{k}" for k in range(len(mid_hooks))]


def _pc_relative(insn):
    """True for instructions that cannot be executed from another address unchanged."""
    return ((insn & 0x7C000000) == 0x14000000      # B / BL
            or (insn & 0xFF000010) == 0x54000000   # B.cond
            or (insn & 0x7E000000) == 0x34000000   # CBZ / CBNZ
            or (insn & 0x7E000000) == 0x36000000   # TBZ / TBNZ
            or (insn & 0x1F000000) == 0x10000000   # ADR / ADRP
            or (insn & 0x3B000000) == 0x18000000)  # LDR (literal)
SDK_SYMS = {
    "SDK_OFF_OpenFile": "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci",
    "SDK_OFF_RoInitialize": "_ZN2nn2ro10InitializeEv",
    "SDK_OFF_RoRegisterModuleInfo": "_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv",
    "SDK_OFF_RoGetBufferSize": "_ZN2nn2ro13GetBufferSizeEPmPKv",
    "SDK_OFF_RoLoadModule": "_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi",
    "SDK_OFF_RoLookupSymbol": "_ZN2nn2ro12LookupSymbolEPmPKc",
}
FORBIDDEN_RELOCS = {257, 258, 259}  # R_AARCH64_ABS64/ABS32/ABS16: would hard-code a link-time address


def _dyn_tags(img):
    mod0 = struct.unpack_from("<I", img, 4)[0]
    dyn = mod0 + struct.unpack_from("<i", img, mod0 + 4)[0]
    tags, i = {}, dyn
    while True:
        t, v = struct.unpack_from("<qQ", img, i)
        i += 16
        if t == 0:
            return tags
        tags.setdefault(t, v)


def analyze_main(main_path, mid_hooks):
    raw = open(main_path, "rb").read()
    img = nso_tool.load_nso_image(main_path)
    text_size = struct.unpack_from("<I", raw, 0x18)[0]
    ro_va = struct.unpack_from("<I", raw, 0x24)[0]
    build_id = raw[0x40:0x60].hex().upper()
    tags = _dyn_tags(img)
    symtab, strtab = tags[6], tags[5]

    def sym(idx):
        st_name, = struct.unpack_from("<I", img, symtab + 24 * idx)
        end = img.index(b"\0", strtab + st_name)
        return img[strtab + st_name:end].decode("latin1")

    got = {}
    for base, size in ((tags[0x17], tags[2]), (tags.get(7, 0), tags.get(8, 0))):
        for k in range(size // 24):
            off, info, _add = struct.unpack_from("<QQq", img, base + 24 * k)
            if info >> 32:
                name = sym(info >> 32)
                if name in PLT_IMPORTS:
                    got[name] = off
    missing = [n for n in PLT_IMPORTS if n not in got]
    if missing:
        raise SystemExit(f"[!] main does not import: {missing}")

    # PLT stub = adrp x16, GOT@page ; ldr x17, [x16, GOT@lo] ; add x16, x16, GOT@lo ; br x17
    plt = {}
    want = {v: k for k, v in got.items()}
    for a in range(0, text_size - 8, 4):
        i1 = struct.unpack_from("<I", img, a)[0]
        if (i1 & 0x9F00001F) != 0x90000010:
            continue
        i2 = struct.unpack_from("<I", img, a + 4)[0]
        if (i2 & 0xFFC003FF) != 0xF9400211:
            continue
        imm = (((i1 >> 5) & 0x7FFFF) << 2 | ((i1 >> 29) & 3)) << 12
        if imm & (1 << 32):
            imm -= 1 << 33
        target = (a & ~0xFFF) + imm + ((i2 >> 10) & 0xFFF) * 8
        if target in want:
            plt[want[target]] = a
            if len(plt) == len(PLT_IMPORTS):
                break
    if len(plt) != len(PLT_IMPORTS):
        raise SystemExit(f"[!] PLT stubs not found for: {[n for n in PLT_IMPORTS if n not in plt]}")

    for name, va, insn in mid_hooks:
        cur = struct.unpack_from("<I", img, va)[0]
        if cur != insn:
            raise SystemExit(f"[!] mid hook {name}: main+0x{va:x} is {cur:08x}, expected {insn:08x} (other game build?)")
        if _pc_relative(insn):
            raise SystemExit(f"[!] mid hook {name}: displaced instruction {insn:08x} is PC-relative")

    malloc = next((v for n, d, v, _i in nso_tool.dynamic_symbols(img) if d and n == "malloc"), None)
    if malloc is None:
        raise SystemExit("[!] main does not export malloc")
    return {"build_id": build_id, "text_size": text_size, "ro_va": ro_va, "got": got, "plt": plt, "malloc": malloc}


def _elf_rel_types(obj_path):
    elf = open(obj_path, "rb").read()
    shoff, = struct.unpack_from("<Q", elf, 0x28)
    shentsize, shnum, _ = struct.unpack_from("<HHH", elf, 0x3A)
    types = set()
    for i in range(shnum):
        sh = struct.unpack_from("<IIQQQQIIQQ", elf, shoff + i * shentsize)
        if sh[1] == 4:  # SHT_RELA
            for k in range(sh[5] // 24):
                _o, info, _a = struct.unpack_from("<QQq", elf, sh[4] + k * 24)
                types.add(info & 0xFFFFFFFF)
    return types


def build(nx_dir, out_dir, profile, clang, lld, mount="Contents"):
    llvm_bin = os.path.dirname(clang)
    exe = ".exe" if os.name == "nt" else ""
    objcopy = os.path.join(llvm_bin, "llvm-objcopy" + exe)
    nm = os.path.join(llvm_bin, "llvm-nm" + exe)
    boot_dir = os.path.join(nx_dir, "boot")
    os.makedirs(out_dir, exist_ok=True)

    m = profile["main"]
    syms_off = profile["sdk"]["syms"]
    offs = {macro: syms_off[name] for macro, name in SDK_SYMS.items()}
    mid_hooks = profile["mid_hooks"]
    boot_va = (m["text_size"] + 0xF) & ~0xF
    limit = m["ro_va"]  # the .text page tail is mapped R-X but unused

    with open(os.path.join(out_dir, "boot_config.h"), "w", encoding="utf-8") as f:
        f.write("// Generated by tools/nro_bootstrap.py. Do not edit.\n#pragma once\n")
        for k, v in offs.items():
            f.write(f"#define {k} 0x{v:x}ul\n")
        f.write(f'#define FEFW_MOUNT "{mount}"\n')
        f.write(f'#define FEFW_NRO_PATH "{mount}:/fefw/runtime_patcher.nro"\n')
        f.write(f'#define FEFW_NRR_PATH "{mount}:/fefw/runtime_patcher.nrr"\n')
        # Mid hook names ("fefw_mid_<k>", mid_hook_exports) are built by boot.c's hook_name(), not stored per hook.
        if len(mid_hooks) > 100:
            raise SystemExit("[!] hook_name() spells at most two digits: 100 mid hooks max")
        f.write(f"#define FEFW_PLT_HOOK_COUNT {len(PLT_HOOKS)}ul\n#define FEFW_MID_HOOK_COUNT {len(mid_hooks)}ul\n")
        f.write("#define FEFW_HOOK_NAME_CASES \\\n")
        f.write("".join(f'    case {i}: return "{exp}"; \\\n' for i, (_, exp) in enumerate(PLT_HOOKS)) + "\n")

    with open(os.path.join(out_dir, "boot_hooks.inc"), "w", encoding="utf-8") as f:
        f.write("// Generated by tools/nro_bootstrap.py. Do not edit.\n")
        f.write("".join(f"    STUB {i}\n" for i in range(len(PLT_HOOKS))))
        f.write("".join(f"    ORIG {i}\n" for i in range(len(PLT_HOOKS))))
        for k, (_name, _va, insn) in enumerate(mid_hooks):
            f.write(f"    MID {k}, {len(PLT_HOOKS) + k}, 0x{insn:08x}\n")

    flags = ["--target=aarch64-none-elf", "-march=armv8-a", "-Os", "-g0", "-ffreestanding", "-fno-builtin",
             "-fno-pic", "-mcmodel=small", "-mgeneral-regs-only", "-fno-stack-protector", "-fno-unwind-tables",
             "-fno-asynchronous-unwind-tables", "-fno-jump-tables", "-nostdinc", "-I", out_dir]
    objs = []
    for src in ("boot_stubs.S", "boot.c"):
        obj = os.path.join(out_dir, src + ".o")
        p = subprocess.run([clang] + flags + ["-c", os.path.join(boot_dir, src), "-o", obj],
                           capture_output=True, text=True)
        if p.returncode != 0:
            raise SystemExit(f"[!] bootstrap compile failed ({src}):\n{p.stderr}")
        bad = _elf_rel_types(obj) & FORBIDDEN_RELOCS
        if bad:
            raise SystemExit(f"[!] {src}: absolute relocations {sorted(bad)} are not allowed in the bootstrap")
        objs.append(obj)

    elf = os.path.join(out_dir, "boot.elf")
    defs = ["--defsym", f"FEFW_BOOT_VA=0x{boot_va:x}", "--defsym", f"fefw_main_malloc=0x{m['malloc']:x}"]
    for i, name in enumerate(PLT_IMPORTS):
        defs += ["--defsym", f"fefw_got_{i}=0x{m['got'][name]:x}"]
    for k, (_name, va, _insn) in enumerate(mid_hooks):
        defs += ["--defsym", f"fefw_mid_ret_{k}=0x{va + 4:x}"]
    p = subprocess.run([lld, "-static", "--no-pie", "-T", os.path.join(boot_dir, "boot.ld"), "-o", elf] + defs + objs,
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit(f"[!] bootstrap link failed:\n{p.stdout}{p.stderr}")
    blob_path = os.path.join(out_dir, "boot.bin")
    subprocess.run([objcopy, "-O", "binary", "--only-section=.boot", elf, blob_path], check=True)
    blob = open(blob_path, "rb").read()
    if boot_va + len(blob) > limit:
        raise SystemExit(f"[!] bootstrap is 0x{len(blob):x} bytes; only 0x{limit - boot_va:x} free in main's .text tail")
    print(f"[+] bootstrap 0x{len(blob):x} bytes, {len(mid_hooks)} mid hook site(s); 0x{limit - boot_va - len(blob):x} of main's .text tail left")

    syms = {}
    for line in subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)

    def branch(src, dst):
        return struct.pack("<I", 0x14000000 | (((dst - src) >> 2) & 0x3FFFFFF))

    records = [(boot_va, blob)]
    for i, name in enumerate(PLT_IMPORTS):
        records.append((m["plt"][name], branch(m["plt"][name], syms[f"fefw_boot_stub_{i}"])))
    for k, (_name, va, _insn) in enumerate(mid_hooks):
        records.append((va, branch(va, syms[f"fefw_boot_mid_{k}"])))

    ips = bytearray(b"IPS32")
    for va, data in records:
        for k in range(0, len(data), 0xFFFF):
            chunk = data[k:k + 0xFFFF]
            ips += struct.pack(">IH", va + 0x100 + k, len(chunk)) + chunk
    ips += b"EEOF"
    print(f"[+] NRO bootstrap: 0x{len(blob):x} bytes, {len(PLT_IMPORTS)} PLT redirects, {len(mid_hooks)} mid hooks")
    return m["build_id"], bytes(ips), records


def ips_ranges(path):
    d = open(path, "rb").read()
    if d[:5] != b"IPS32":
        return []
    i, out = 5, []
    while d[i:i + 4] != b"EEOF":
        off = int.from_bytes(d[i:i + 4], "big") - 0x100
        size = int.from_bytes(d[i + 4:i + 6], "big")
        out.append((off, off + size))
        i += 6 + size
    return out
