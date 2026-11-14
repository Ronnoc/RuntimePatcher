# .delta / .str / .bntx package writer. Keep the formats in sync
# with src/vfs_overlay.cpp (the C++ reader of .delta / .str / .bntx packages).
#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
FEFW Delta & Decoupled Class Specification Engine (Path 1 Architecture)
======================================================================
Defines binary packaging, serialization, and deserialization for decoupled
class and skill mods in Fire Emblem: Fortune's Weave.

Guarantees 100% Zero-Conflict LayeredFS:
  - Output files are placed in romfs/GameData/Classes/<mod_id>.*
  - No mod touches global Job.bin, Skill.bin, AssetTable.bin, Icon_Job.arc.zst, or Msg/*.
"""

import os
import sys
import json
import struct
from typing import Dict, List, Any, Optional

def fnv1_64(data: bytes, basis: int = 0xCBF29CE484222325) -> int:
    """FNV-1 (multiply, then xor) — identical to FEFWTwiddler Utils.BitMath.fnv1_64 / the game's hash.
    Not FNV-1a: label/mod hashes must match what the game and src/vfs_overlay.cpp expect."""
    h = basis
    for b in data:
        h = ((h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF) ^ b
    return h


def fnv1_32(data: bytes) -> int:
    return fnv1_64(data) & 0xFFFFFFFF


# ============================================================================
# Binary Formats Magic & Constants
# ============================================================================
DELTA_MAGIC = b"FWDL"       # FEFW Delta Container
DELTA_VERSION = 0x00010000   # Version 1.0

STRING_MAGIC = b"FWST"      # FEFW String Table Container
STRING_VERSION = 0x00010000  # Version 1.0

STANDARD_LANGUAGES = [
    "CNzh", "TWzh", "JPja", "USen", "EUen", "KRko",
    "EUfr", "USfr", "EUde", "EUes", "USes", "EUit"
]


class DeltaPackage:
    """
    Handles serialization and validation of .delta files.
    """
    @staticmethod
    def pack(
        manifest: Dict[str, Any],
        output_path: str,
        binary_job_rows: Optional[bytes] = None,
        binary_skill_rows: Optional[bytes] = None,
        binary_asset_rows: Optional[bytes] = None
    ) -> bytes:
        """Packs a manifest and optional binary row buffers into a .delta file."""
        mod_id = manifest.get("id", "custom_class")
        mod_hash = fnv1_32(mod_id.encode("utf-8"))

        flags = 0
        if manifest.get("custom_skills"):
            flags |= 1
        if any("relink_progression" in c for c in manifest.get("classes", [])):
            flags |= 2
        if any("assets" in c for c in manifest.get("classes", [])):
            flags |= 4
        if binary_job_rows:
            flags |= 8

        # Clean copy of manifest metadata for header (strip internal build flags)
        clean_manifest = {k: v for k, v in manifest.items() if not k.startswith("_")}
        json_header_bytes = json.dumps(clean_manifest, ensure_ascii=False, indent=2).encode("utf-8")

        hdr_fmt = "<4s11I16s"
        hdr_size = struct.calcsize(hdr_fmt)  # 64 bytes

        json_off = hdr_size
        json_sz = len(json_header_bytes)

        curr_off = json_off + json_sz
        # Align to 8 bytes
        if curr_off % 8 != 0:
            pad = 8 - (curr_off % 8)
            json_header_bytes += b"\x00" * pad
            curr_off += pad

        job_off = curr_off if binary_job_rows else 0
        job_sz = len(binary_job_rows) if binary_job_rows else 0
        curr_off += job_sz

        skill_off = curr_off if binary_skill_rows else 0
        skill_sz = len(binary_skill_rows) if binary_skill_rows else 0
        curr_off += skill_sz

        asset_off = curr_off if binary_asset_rows else 0
        asset_sz = len(binary_asset_rows) if binary_asset_rows else 0

        header_bytes = struct.pack(
            hdr_fmt,
            DELTA_MAGIC,
            DELTA_VERSION,
            flags,
            mod_hash,
            json_off,
            json_sz,
            job_off,
            job_sz,
            skill_off,
            skill_sz,
            asset_off,
            asset_sz,
            b"\x00" * 16
        )

        payload = bytearray()
        payload.extend(header_bytes)
        payload.extend(json_header_bytes)
        if binary_job_rows:
            payload.extend(binary_job_rows)
        if binary_skill_rows:
            payload.extend(binary_skill_rows)
        if binary_asset_rows:
            payload.extend(binary_asset_rows)

        os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
        with open(output_path, "wb") as f:
            f.write(payload)

        return bytes(payload)

    @staticmethod
    def unpack(path: str) -> Dict[str, Any]:
        """Parses and validates a .delta file."""
        with open(path, "rb") as f:
            data = f.read()

        hdr_fmt = "<4s11I16s"
        hdr_size = struct.calcsize(hdr_fmt)
        if len(data) < hdr_size:
            raise ValueError(f"File {path} is too small for a valid .delta file.")

        magic, ver, flags, mod_hash, j_off, j_sz, job_off, job_sz, sk_off, sk_sz, as_off, as_sz, _ = struct.unpack_from(
            hdr_fmt, data, 0
        )
        if magic != DELTA_MAGIC:
            raise ValueError(f"Invalid magic: {magic}, expected {DELTA_MAGIC}")
        if ver != DELTA_VERSION:
            raise ValueError(f"Unsupported delta version: 0x{ver:08X}")

        json_bytes = data[j_off : j_off + j_sz]
        manifest = json.loads(json_bytes.decode("utf-8"))

        return {
            "magic": magic.decode("ascii"),
            "version": ver,
            "flags": flags,
            "mod_id_hash": mod_hash,
            "manifest": manifest,
            "binary_job_rows": data[job_off : job_off + job_sz] if job_sz > 0 else b"",
            "binary_skill_rows": data[sk_off : sk_off + sk_sz] if sk_sz > 0 else b"",
            "binary_asset_rows": data[as_off : as_off + as_sz] if as_sz > 0 else b""
        }


class StringPackage:
    """
    Handles serialization and extraction of multi-language string tables (.str).
    Encodes 12 standard Switch languages in UTF-16LE for native AArch64 string fallback.
    """
    @staticmethod
    def pack(manifest: Dict[str, Any], output_path: str) -> bytes:
        """Packs localizations for all classes and skills across 12 languages."""
        entries = []

        # 1. Class name & help entries
        for c in manifest.get("classes", []):
            name_id = c.get("name_id")
            help_id = c.get("help_id")
            if name_id:
                entries.append((name_id, "name", manifest.get("localization", {})))
            if help_id:
                entries.append((help_id, "help", manifest.get("localization", {})))

        # 2. Custom Skill name & help entries
        for sk in manifest.get("custom_skills", []):
            sk_name_id = sk.get("name_id")
            sk_help_id = sk.get("help_id")
            loc = sk.get("localization", {})
            if sk_name_id:
                entries.append((sk_name_id, "name", loc))
            if sk_help_id:
                entries.append((sk_help_id, "help", loc))

        # Build table structure
        # Header: 'FWST', version, lang_count (12), entry_count
        # Lang Table: 12 entries -> [code (4B), offset (4B), size (4B)]
        lang_blobs = {}
        for lang in STANDARD_LANGUAGES:
            blob = bytearray()
            for label, field_name, loc_dict in entries:
                lang_data = loc_dict.get(lang) or loc_dict.get("USen", {})
                text_val = lang_data.get(field_name, "")
                text_u16 = text_val.encode("utf-16le")

                label_bytes = label.encode("ascii") + b"\x00"
                while len(label_bytes) % 4 != 0:
                    label_bytes += b"\x00"

                lbl_hash = fnv1_32(label.encode("utf-8"))
                entry_hdr = struct.pack("<III", lbl_hash, len(label_bytes), len(text_u16))
                blob.extend(entry_hdr)
                blob.extend(label_bytes)
                blob.extend(text_u16)
                while len(blob) % 4 != 0:
                    blob.append(0)
            lang_blobs[lang] = bytes(blob)

        hdr_fmt = "<4sIII"
        hdr_sz = struct.calcsize(hdr_fmt)
        lang_entry_fmt = "<4sII"
        lang_table_sz = len(STANDARD_LANGUAGES) * struct.calcsize(lang_entry_fmt)

        curr_off = hdr_sz + lang_table_sz
        lang_table_bytes = bytearray()
        data_bytes = bytearray()

        for lang in STANDARD_LANGUAGES:
            blob = lang_blobs[lang]
            off = curr_off
            sz = len(blob)
            lang_code_bytes = lang.encode("ascii")
            if len(lang_code_bytes) < 4:
                lang_code_bytes += b"\x00" * (4 - len(lang_code_bytes))
            lang_table_bytes.extend(struct.pack(lang_entry_fmt, lang_code_bytes[:4], off, sz))
            data_bytes.extend(blob)
            curr_off += sz

        hdr_bytes = struct.pack(hdr_fmt, STRING_MAGIC, STRING_VERSION, len(STANDARD_LANGUAGES), len(entries))

        payload = bytearray()
        payload.extend(hdr_bytes)
        payload.extend(lang_table_bytes)
        payload.extend(data_bytes)

        os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
        with open(output_path, "wb") as f:
            f.write(payload)

        return bytes(payload)

    @staticmethod
    def unpack(path: str) -> Dict[str, Any]:
        """Parses and validates a .str file."""
        with open(path, "rb") as f:
            data = f.read()

        hdr_fmt = "<4sIII"
        hdr_sz = struct.calcsize(hdr_fmt)
        magic, ver, num_langs, num_entries = struct.unpack_from(hdr_fmt, data, 0)
        if magic != STRING_MAGIC:
            raise ValueError(f"Invalid string magic: {magic}, expected {STRING_MAGIC}")

        lang_entry_fmt = "<4sII"
        l_entry_sz = struct.calcsize(lang_entry_fmt)

        result = {
            "magic": magic.decode("ascii"),
            "version": ver,
            "num_languages": num_langs,
            "entry_count": num_entries,
            "languages": {}
        }

        for i in range(num_langs):
            code_b, off, sz = struct.unpack_from(lang_entry_fmt, data, hdr_sz + i * l_entry_sz)
            lang_code = code_b.decode("ascii").rstrip("\x00")
            lang_data = {}

            blob = data[off : off + sz]
            p = 0
            while p < len(blob):
                lbl_hash, lbl_len, text_len = struct.unpack_from("<III", blob, p)
                p += 12
                lbl_str = blob[p : p + lbl_len].decode("ascii").rstrip("\x00")
                p += lbl_len
                text_str = blob[p : p + text_len].decode("utf-16le")
                p += text_len
                while p % 4 != 0 and p < len(blob):
                    p += 1
                lang_data[lbl_str] = text_str

            result["languages"][lang_code] = lang_data

        return result


def export_decoupled_mod_package(
    manifest: Dict[str, Any],
    dist_classes_dir: str,
    custom_bntx_bytes: Optional[bytes] = None
) -> Dict[str, str]:
    """
    Exports a standalone, conflict-free decoupled class package according to Path 1.
    Writes:
      - <mod_id>.delta
      - <mod_id>.str
      - <mod_id>.bntx
    """
    mod_id = manifest.get("id", "custom_class")
    os.makedirs(dist_classes_dir, exist_ok=True)

    delta_file = os.path.join(dist_classes_dir, f"{mod_id}.delta")
    str_file = os.path.join(dist_classes_dir, f"{mod_id}.str")
    bntx_file = os.path.join(dist_classes_dir, f"{mod_id}.bntx")

    # 1. Delta file
    DeltaPackage.pack(manifest, delta_file)

    # 2. String file
    StringPackage.pack(manifest, str_file)

    # 3. BNTX icon file
    if custom_bntx_bytes is None:
        # Default placeholder BNTX container (40 bytes header)
        bntx_magic = b"BNTX\x00\x00\x00\x00"
        mod_hash = fnv1_32(mod_id.encode("utf-8"))
        custom_bntx_bytes = bntx_magic + struct.pack("<II16s", 0x1000, mod_hash, b"\x00" * 16)

    with open(bntx_file, "wb") as f:
        f.write(custom_bntx_bytes)

    return {
        "delta": delta_file,
        "str": str_file,
        "bntx": bntx_file
    }
