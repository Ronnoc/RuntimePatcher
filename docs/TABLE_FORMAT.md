# GameData table format (`*.bin`)

Notes on the table container used by `GameData/*.bin` and similar files, as implemented by
`src/game_table.cpp`. All integers are little-endian. "Pointer" means a u64 holding `target - 32`; offset 0 is
never a valid target, so 0 means null.

## File layout

```
0x00  u32  file size
0x04  u32  relocation list offset (relative to 0x20)
0x08  u32  relocation count
0x0C  u32  key index count
0x10  u32  0
0x14  u32  1
0x18  8 bytes zero
0x20  sheet 0, sheet 1, ...          (body: everything pointers point into)
      relocation list: u64 (pos - 32) of every pointer slot in the body
      key index: per entry u32 (record pos - 32), u32 (key offset inside the string pool)
      string pool: NUL-terminated UTF-8 strings
```

A file is recognised as a table when it is at least 64 bytes and its first u32 equals its size.

## Sheet

```
field descriptors   N x 32 bytes
rows                (see below)
array payloads      8-aligned
sheet info          32 bytes: u64 version, ptr name, u32 record size, u32 field count, ptr first descriptor
sheet root          48 bytes: a row header (key = sheet name, count = number of rows or groups), ptr info, ptr rows
```

The key index lists each sheet root, so readers find sheets through it. `version >> 32 == 2` marks a grouped
sheet.

### Field descriptor (32 bytes)

`ptr name`, `ptr type`, `f32 min`, `f32 max`, `u32 size`, `u32 offset` (offset inside the record).

Types: `u8 s8 b8 bool u16 s16 b16 u32 s32 int b32 f32 float color s64 string data ref`, each optionally suffixed
`[]` (array) or `[,]` (array of arrays). `b*` are bit flags, `color` is RGBA8, `string`/`data` are string
pointers, `ref` points at another record of the file and is shown as that record's key.

### Records

A record of a sheet whose smallest field offset is >= 32 starts with a 32-byte row header:
`u64 0`, `ptr key` (null for continuation rows), `u16 index`, `u16 count`, `u32 line`, `u64 0`. Otherwise the
sheet is *headerless* and records hold only fields.

- Flat sheet: records follow each other (stride = record size). A row without a key continues the previous key.
- Grouped sheet: first all group headers (a row header + ptr to the group's first record, plus 8 bytes padding
  when the sheet has row headers: stride 48, else 40), then each group's records back to back.

Arrays: the field slot holds `u16 count`, `u16 element size`, 4 bytes padding, ptr payload. An array of arrays
points at `count` such 16-byte slots. Identical payloads may be shared.

## Older files without usable sheet roots

When no key-index entry yields a consistent sheet root, sheets are found by scanning: the first starts at 0x20,
later ones are located 8-aligned where two consecutive plausible field descriptors start (and the 32 bytes
before are not one). Rows are read while a row header with the expected running index is present; grouped sheets
are recognised from their group headers.

## Writing

The writer emits sheets in order, then patches `ref` slots to the position of the record with that key, then
appends the relocation list (non-string pointers in address order, then string pointers grouped by text in order
of first use), the key index (first occurrence of each key: rows with a key, group headers, sheet roots) and the
string pool (index keys first, then other strings in order of first use, each stored once).
