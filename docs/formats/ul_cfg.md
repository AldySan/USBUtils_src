# ul.cfg — USBExtreme game index format

**Derived from:** `BuildRecord @0x39B518`, `SaveULCfg @0x39DA20`,
`DecodeRecordName @0x39D84B`, `GetIdString @0x39A38A` (disassembly).

The file `ul.cfg` lives in the **root of the FAT32 USB drive** and is a
plain concatenation of fixed-size records:

```
ul.cfg := record*                ; no header, no checksum
record := 64 bytes               ; little-endian where applicable
```

## Record layout (64 bytes)

| Offset | Size | Field | Value / notes |
|---:|---:|---|---|
| 0x00 | 32 | `name[32]` | game display name, NUL/space padded (first 32 chars of the name) |
| 0x20 | 3 | `magic` | ASCII `"ul."` (0x75 0x6C 0x2E) |
| 0x23 | 11 | `serial[11]` | game serial from `SYSTEM.CNF` BOOT2 path, e.g. `SLUS_202.65`, NUL padded (max 11 chars) |
| 0x2E | 1 | `reserved` | 0 |
| 0x2F | 1 | `parts` | number of chunk files (u8) |
| 0x30 | 1 | `media` | `0x12` = CD, `0x14` = DVD |
| 0x31 | 4 | `reserved` | 0 |
| 0x35 | 1 | `flag` | constant **8** in USBUtil-written records |
| 0x36 | 10 | `reserved` | 0 |

Equivalent C struct (this repo's `ulcfg.h`):

```c
#pragma pack(push,1)
typedef struct {
    char     name[32];   /* 0x00 */
    char     magic[3];   /* 0x20 "ul." */
    char     serial[11]; /* 0x23 */
    uint8_t  pad0;       /* 0x2E */
    uint8_t  parts;      /* 0x2F */
    uint8_t  media;      /* 0x30 0x12 CD / 0x14 DVD */
    uint8_t  pad1[4];    /* 0x31 */
    uint8_t  flag;       /* 0x35 = 8 */
    uint8_t  pad2[10];   /* 0x36 */
} ul_cfg_record;         /* sizeof == 64 */
#pragma pack(pop)
```

## Media selection rule (from `GetMediaType @0x39D980`)

1. If the source was flagged CD in the UI → `0x12`.
2. Else if flagged DVD in the UI → `0x14`.
3. Else by image size: `size <= 734,003,200` (700 MB) → `0x12`, else `0x14`.

## Write behaviour (from `SaveULCfg @0x39DA20`)

* **Game list loaded** (ul.cfg exists and the in-memory list is active):
  the whole file is rewritten, with the new record inserted at the chosen
  list position.
* **No list loaded**:
  * ul.cfg exists → open, seek to end, append the single new record.
  * ul.cfg missing → create the file and write the record.
* A `ul-old.cfg` backup path exists in the code (Spanish string
  `ul-old.cfg` @0x982E8) — the original keeps a `.old` style copy during
  certain repair operations.

## Reading quirks (from `DecodeRecordName @0x39D84B`)

The original displays a record by concatenating `trim_right(name[0..31])`
with the tail of the record and subtracting `0x30` from bytes 46..63 —
that path is only used by the "View/Edit ul.cfg" debug view; real game
entries are parsed field-by-field as in the table above.

## Game ID embedded in chunk filenames

The chunk filenames embed a CRC-32 of the *trimmed game name + NUL*:

```
id = CRC32_msb( Trim(name) + "\x00" )    ; poly 0x04C11DB7, init 0,
                                         ; no reflection, no final xor
id_hex = uppercase hexadecimal, 8 digits  (IntToHex(crc, 8))
```

See `formats/file_naming.md` for how `id_hex` and `serial` compose file names.
