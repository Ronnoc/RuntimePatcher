#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
FEFW RuntimePatcher — NSO utility
===================================
  syms     <nso> [--imports|--exports] [--grep REGEX]   Dump dynamic symbols of an NSO (main/sdk/subsdkN)
  elf2nso  <elf> <out_nso>                              Convert a linked aarch64 ELF shared object into an
                                                         uncompressed NSO0 module (for exefs/subsdk9)
"""

import argparse
import hashlib
import os
import re
import struct
import sys



# ----------------------------------------------------------------------------------------------
# NSO -> flat image
# ----------------------------------------------------------------------------------------------
def _nso_decompress(blob: bytes, uncomp_sz: int) -> bytes:
    if blob[:4] == b"ZBIC":  # Switch 2
        import zbic  # pip install zbic
        return zbic.decompress(blob)
    import lz4.block  # pip install lz4
    return lz4.block.decompress(blob, uncompressed_size=uncomp_sz)


class _NSO:
    def __init__(self, path: str):
        with open(path, "rb") as f:
            raw = f.read()
        if raw[:4] != b"NSO0":
            raise ValueError(f"{path}: not an NSO (magic {raw[:4]!r})")
        self.raw = raw
        self.flags = struct.unpack_from("<I", raw, 0x0C)[0]
        self.text_off, self.text_loc, self.text_sz = struct.unpack_from("<III", raw, 0x10)
        self.rodata_off, self.rodata_loc, self.rodata_sz = struct.unpack_from("<III", raw, 0x20)
        self.data_off, self.data_loc, self.data_sz = struct.unpack_from("<III", raw, 0x30)
        self.text_cmp_sz, self.rodata_cmp_sz, self.data_cmp_sz = struct.unpack_from("<III", raw, 0x60)

    def _decompress(self, off, cmp_sz, sz):
        return _nso_decompress(self.raw[off:off + cmp_sz], sz)


def load_nso_image(path: str) -> bytearray:
    n = _NSO(path)

    def seg(off, cmp_sz, sz, bit):
        if n.flags & bit:
            return n._decompress(off, cmp_sz, sz)
        return n.raw[off:off + sz]

    t = seg(n.text_off, n.text_cmp_sz, n.text_sz, 1)
    ro = seg(n.rodata_off, n.rodata_cmp_sz, n.rodata_sz, 2)
    d = seg(n.data_off, n.data_cmp_sz, n.data_sz, 4)
    img = bytearray(n.data_loc + n.data_sz)
    img[n.text_loc:n.text_loc + len(t)] = t
    img[n.rodata_loc:n.rodata_loc + len(ro)] = ro
    img[n.data_loc:n.data_loc + len(d)] = d
    return img


def dynamic_symbols(img: bytes):
    mod0 = struct.unpack_from("<I", img, 4)[0]
    if img[mod0:mod0 + 4] != b"MOD0":
        raise ValueError("MOD0 header not found")
    dyn = mod0 + struct.unpack_from("<i", img, mod0 + 4)[0]
    tags = {}
    i = dyn
    while True:
        tag, val = struct.unpack_from("<qQ", img, i)
        i += 16
        if tag == 0:
            break
        tags.setdefault(tag, val)
    symtab, strtab = tags[6], tags[5]
    if 4 in tags:  # DT_HASH -> nchain
        nsyms = struct.unpack_from("<I", img, tags[4] + 4)[0]
    else:
        nsyms = (strtab - symtab) // 24
    out = []
    for k in range(1, nsyms):
        name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", img, symtab + k * 24)
        end = img.index(0, strtab + name)
        out.append((img[strtab + name:end].decode("latin1"), shndx != 0, value, info))
    return out


def cmd_syms(args):
    img = load_nso_image(args.nso)
    rx = re.compile(args.grep) if args.grep else None
    for name, defined, value, info in dynamic_symbols(img):
        if args.imports and defined:
            continue
        if args.exports and not defined:
            continue
        if rx and not rx.search(name):
            continue
        print(f"{'EXP' if defined else 'IMP'} {value:#010x} {name}")


# ----------------------------------------------------------------------------------------------
# ELF -> NSO
# ----------------------------------------------------------------------------------------------
PF_X, PF_W, PF_R = 1, 2, 4


def cmd_elf2nso(args):
    elf = open(args.elf, "rb").read()
    if elf[:4] != b"\x7fELF" or elf[4] != 2:
        raise SystemExit("not an ELF64 file")
    e_phoff, = struct.unpack_from("<Q", elf, 0x20)
    e_phentsize, e_phnum = struct.unpack_from("<HH", elf, 0x36)

    loads = []
    for i in range(e_phnum):
        p_type, p_flags, p_offset, p_vaddr, _p_paddr, p_filesz, p_memsz, _p_align = struct.unpack_from(
            "<IIQQQQQQ", elf, e_phoff + i * e_phentsize)
        if p_type == 1:  # PT_LOAD
            loads.append((p_flags, p_offset, p_vaddr, p_filesz, p_memsz))
    text = [l for l in loads if l[0] & PF_X]
    ro = [l for l in loads if not (l[0] & PF_X) and not (l[0] & PF_W)]
    rw = [l for l in loads if l[0] & PF_W]
    if len(text) != 1 or len(ro) != 1 or len(rw) != 1:
        raise SystemExit(f"expected exactly 1 RX, 1 R, 1 RW PT_LOAD (got {len(text)}/{len(ro)}/{len(rw)}); check linker script")
    text, ro, rw = text[0], ro[0], rw[0]
    for seg in (text, ro, rw):
        if seg[2] & 0xFFF:
            raise SystemExit(f"segment vaddr {seg[2]:#x} not page aligned")
    if text[2] != 0:
        raise SystemExit("text segment must start at vaddr 0")

    def body(seg):
        return elf[seg[1]:seg[1] + seg[3]]

    t, r, d = body(text), body(ro), body(rw)
    bss = rw[4] - rw[3]

    hdr = bytearray(0x100)
    hdr[0:4] = b"NSO0"
    struct.pack_into("<I", hdr, 0x0C, 0)  # flags: uncompressed, no hash check
    off = 0x100
    segs = []
    for data, vaddr in ((t, text[2]), (r, ro[2]), (d, rw[2])):
        segs.append((off, vaddr, len(data)))
        off += len(data)
    struct.pack_into("<IIII", hdr, 0x10, segs[0][0], segs[0][1], segs[0][2], 0)
    struct.pack_into("<IIII", hdr, 0x20, segs[1][0], segs[1][1], segs[1][2], 0)
    struct.pack_into("<IIII", hdr, 0x30, segs[2][0], segs[2][1], segs[2][2], bss)
    # build id: sha1 of contents (only used for IPS/cheat matching)
    hdr[0x40:0x54] = hashlib.sha1(t + r + d).digest()
    struct.pack_into("<III", hdr, 0x60, len(t), len(r), len(d))
    for i, data in enumerate((t, r, d)):
        hdr[0xA0 + i * 0x20:0xA0 + i * 0x20 + 0x20] = hashlib.sha256(data).digest()

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(hdr + t + r + d)
    print(f"[elf2nso] {args.out}: text={len(t):#x} ro={len(r):#x}@{ro[2]:#x} "
          f"data={len(d):#x}@{rw[2]:#x} bss={bss:#x}")


def _elf_loads(path):
    elf = open(path, "rb").read()
    if elf[:4] != b"\x7fELF" or elf[4] != 2:
        raise SystemExit("not an ELF64 file")
    e_phoff, = struct.unpack_from("<Q", elf, 0x20)
    e_phentsize, e_phnum = struct.unpack_from("<HH", elf, 0x36)
    loads = []
    for i in range(e_phnum):
        p_type, p_flags, p_offset, p_vaddr, _pa, p_filesz, p_memsz, _al = struct.unpack_from(
            "<IIQQQQQQ", elf, e_phoff + i * e_phentsize)
        if p_type == 1:
            loads.append((p_flags, p_offset, p_vaddr, p_filesz, p_memsz))
    text = [l for l in loads if l[0] & PF_X]
    ro = [l for l in loads if not (l[0] & PF_X) and not (l[0] & PF_W)]
    rw = [l for l in loads if l[0] & PF_W]
    if len(text) != 1 or len(ro) != 1 or len(rw) != 1:
        raise SystemExit("expected exactly 1 RX, 1 R, 1 RW PT_LOAD")
    return elf, text[0], ro[0], rw[0]


def _page(n):
    return (n + 0xFFF) & ~0xFFF


def cmd_elf2nro(args):
    """Flat NRO: text|ro|data contiguous and page aligned, NRO header at text+0x10 (crt0 reserves 0x80 bytes)."""
    elf, text, ro, rw = _elf_loads(args.elf)
    if text[2] != 0:
        raise SystemExit("text segment must start at vaddr 0")
    img = bytearray(_page(rw[2] + rw[3]))
    for _f, off, vaddr, filesz, _m in (text, ro, rw):
        img[vaddr:vaddr + filesz] = elf[off:off + filesz]
    if img[0x10:0x80] != bytes(0x70):
        raise SystemExit("bytes 0x10..0x80 must be reserved (zero) for the NRO header; use nx/probe/nro_crt0.S")
    text_size, ro_size = ro[2], rw[2] - ro[2]
    data_size = len(img) - rw[2]
    bss = _page(rw[2] + rw[4]) - len(img)
    if bss < 0:
        bss = 0
    struct.pack_into("<4sIII", img, 0x10, b"NRO0", 0, len(img), 0)
    struct.pack_into("<IIIIIII", img, 0x20, 0, text_size, ro[2], ro_size, rw[2], data_size, bss)
    img[0x40:0x60] = hashlib.sha256(bytes(img[0x80:])).digest()  # build id
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    open(args.out, "wb").write(img)
    print(f"[elf2nro] {args.out}: size={len(img):#x} text={text_size:#x} ro={ro_size:#x} data={data_size:#x} bss={bss:#x}")


def cmd_mknrr(args):
    """NRR0 registration list for ldr:ro (unsigned; emulators do not verify the RSA signatures).

    --format 1 (default): layout required by this game's sdk (the sdk's nn::ro parser):
        0x000 "NRR0", 0x005 = 1, 0x006 = 1 (format; the only accepted value), 0x010 certification header
        (0x020 = 1 -> 0x180-byte keys), 0x030 modulus[0x180], 0x1B0 cert signature[0x180],
        0x330 NRR signature[0x180], 0x4B0 body: program_id u64, size u32, type u8, pad[3],
        hashes_offset u32 (+0x10, must be 0x4D0), num_hashes u32, reserved[8]; hashes at 0x4D0.
    --format 0: classic Switch 1 layout (0x100-byte keys, body at 0x330, header 0x350).
    """
    hashes = sorted(hashlib.sha256(open(n, "rb").read()).digest() for n in args.nro)
    pid = int(args.program_id, 16)
    if args.format == 1:
        body, hdr_size = 0x4B0, 0x4D0
    else:
        body, hdr_size = 0x330, 0x350
    size = _page(hdr_size + 0x20 * len(hashes))
    nrr = bytearray(size)
    nrr[0:4] = b"NRR0"
    if args.format == 1:
        nrr[5] = 1
        nrr[6] = 1
        nrr[0x20] = 1
    struct.pack_into("<QQ", nrr, 0x10, 0xFFFFFFFFFFFFFFFF, pid) if args.format == 0 else struct.pack_into("<Q", nrr, 0x18, pid)
    struct.pack_into("<QIB", nrr, body, pid, size, 0)           # program_id, size, type (0 = user NRO)
    struct.pack_into("<II", nrr, body + 0x10, hdr_size, len(hashes))
    for k, h in enumerate(hashes):
        nrr[hdr_size + k * 0x20:hdr_size + (k + 1) * 0x20] = h
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    open(args.out, "wb").write(nrr)
    print(f"[mknrr] {args.out}: format {args.format}, {len(hashes)} hash(es), program id {pid:016X}")

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("syms")
    s.add_argument("nso")
    s.add_argument("--imports", action="store_true")
    s.add_argument("--exports", action="store_true")
    s.add_argument("--grep")
    s.set_defaults(fn=cmd_syms)
    e = sub.add_parser("elf2nso")
    e.add_argument("elf")
    e.add_argument("out")
    e.set_defaults(fn=cmd_elf2nso)
    r = sub.add_parser("elf2nro")
    r.add_argument("elf")
    r.add_argument("out")
    r.set_defaults(fn=cmd_elf2nro)
    n = sub.add_parser("mknrr")
    n.add_argument("out")
    n.add_argument("nro", nargs="+")
    n.add_argument("--program-id", default="04007EF01F8AE000")
    n.add_argument("--format", type=int, default=1, choices=[0, 1])
    n.set_defaults(fn=cmd_mknrr)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
