# USBUtil v2.2 rev1.0 — Reverse Engineering Notes

This document records the complete reverse-engineering analysis of
**USBUtil v2.2 rev1.0 (English, April 2014)** by ISEKO (2009), performed on the
original Delphi binary in order to produce this open C/C++ reimplementation.

Original binary SHA-256 see `docs/binary_analysis.md`.
Target: `USBUtil v2.2 rev1.0_English.exe` (1,280,512 bytes).

---

## 1. Binary fingerprint

| Property | Value |
|---|---|
| Format | PE32 GUI executable, Intel i386, 8 sections |
| Compiler | **Embarcadero Delphi** (Unicode-era RTL, `SOFTWARE\Borland\Delphi\RTL` strings present) |
| Image base | `0x00300000` |
| Sections | `CODE @0x301000 (0xBC69C)`, `DATA @0x3BE000`, `BSS`, `.idata`, `.tls`, `.rdata`, `.reloc`, `.rsrc @0x3BD6000` |
| Forms (RCDATA) | `TForm1` (main, 364 KB DFM!), `TForm2`, `TBurnerForm`, `TBurnForm`, `TISOForm` |
| Project name | `Project1` (from PACKAGEINFO) |

Key custom units identified through `PACKAGEINFO` resource:

```
Unit1, ISOUnit, jISOImage, CDBufferedStream, CustomImage, CovertFuncs,
UDeviceTypes, SCSITypes, wnaspi32, SCSIDefs, ISO9660ClassTypes,
ISO9660BootClass, ISO9660PrimaryVolClass, ISO9660SupplementVolClass,
ISO9660MicroUDF* (7 units), MicroUDF*, BinCueImage, BinCueReader, ReadWave,
HandledThread, SCSIUnit, SptiUnit, skSCSI, SCDROMIOCTL, EraserThread,
zDeviceReader, DiscInfo, CDDAText, DiskCDDBInfo, DiskNotifier, Devices,
DeviceNotifier, DeviceHelper, xCDSizer, BurnUnit, CGauges, dShellCtrls, USBHDH*
```

This reveals the original architecture: a full ISO9660/MicroUDF image builder,
an ASPI/SPTI SCSI stack (disc ripping + burning), USB device hotplug
notification, and the USB HDD game management core (`USBHDH*`).

## 2. Feature map (from main menu + popup menus)

* Open/Close games list (`ul.cfg`)
* Open ISO to Explore/Modify
* **Create GAME from ISO** / **Create GAME from CD/DVD**
* Recover games (scan `ul.*.00` orphans), list of recovery (`usbutil.rcn`)
* View/Edit `ul.cfg` (ported as `list`/`rename`/`remove`/`fix`)
* Find file in games (ported: `usbutils find`, wildcard + size search)
* **Patch DNAS/IOPR/IRX** (replace IOPRP images inside installed games)
* **PS2Load options** (swap the game's boot program for PS2LOAD.ELF —
  marker record + MSYSTEM.CNF backup + in-place SYSTEM.CNF rewrite;
  see `formats/ps2load.md`)
* Correct code 666.66 — the original compares 11 chars of each game's
  code column against the broken marker "666.66" (0x39B774..0x39B85C,
  string @0x39B854) and re-derives the real serial from SYSTEM.CNF,
  then renames record + chunks ("No se pudo renombrar codigo 666.66"
  is the failure path). Ported as `usbutils fix`.
* Structure check (verify all chunk files exist)
* Rip GAME (video removal: activate/deactivate/coincide)
* Restore game (partial/total) — covered by `recover` + `unrip`
* Copy/Move games, copy history (ported: `usbutils copy`/`move`), general
  report (ported: `usbutils report`)
* Per-game file explore / extract / "INJECT and match" (ported:
  `usbutils files`/`extract`/`inject` — same-size in-place replacement)
* Create GAME from CD/DVD (POSIX device paths work: `install /dev/sr0`)
* Game name database (`usbutil.ljs` — 1432 tested-game notes)

## 3. Key code addresses (English exe, image base 0x300000)

| Function | VMA | Purpose |
|---|---|---|
| `InitCRCTable` | `0x398738` | builds 256-entry CRC-32 table, poly **0x04C11DB7**, MSB-first |
| `ComputeGameId` | `0x398784` | `CRC32(Trim(name)+#0)` → `IntToHex(crc,8)` |
| `DecodeRecordName` | `0x39D84B` | record string → display name (trailing-space trim, `-0x30` on bytes 46..63) |
| `BuildRecord` | `0x39B518` | build the 64-byte `ul.cfg` record (authoritative layout, see below) |
| `GetIdString` | `0x39A38A` | record bytes 35..45 → ID string (11 chars) |
| `SaveULCfg` | `0x39DA20` | write `ul.cfg`: rewrite-all-with-insert or append |
| `CreateGameRecord` | `0x39DCB0` | per-chunk copy loop + record creation + save |
| `WriteChunkFile` | `0x39CF44` | raw copy with 8 MiB buffer, progress + cancel |
| `SplitISO` | `0x3A112F` | parts = ceil(size/1 GiB), chunk writing loop |
| `ParseSystemCnf` | `0x3A0F10` | locate SYSTEM.CNF via ISO9660 dir (LBA×2048), extract serial |
| `ScanULPattern` | `0x3A2B77` | `FindFirst(path + "ul.*.00")` |
| `GetMediaType` | `0x39D980` | CD (0x12) if size ≤ 700 MB else DVD (0x14) |
| `rcn save` | `0x39306B` | recovery DB encoder (see formats/recovery_rcn.md) |

## 4. Important constants

| Constant | Value | Meaning |
|---|---|---|
| Chunk size | `0x40000000` (1 GiB) | max size of one `ul.*.NN` part |
| CD threshold | 734 003 200 (700 MB) | ≤ threshold ⇒ media = CD |
| Media codes | `0x12` CD, `0x14` DVD | byte 48 of record |
| Record magic | `"ul."` at bytes 32..34 | prefix stored inside the record |
| Record byte 53 | `8` | constant flag |
| Copy buffer | 8 MiB + 32 B | global BSS buffer |
| Sector size | 2048 | ISO9660 logical block size |

## 5. String-constant pool layout (Delphi Unicode-era)

AnsiString constants in `CODE` are stored as:

```
FFFFFFFF  <u32 length>  <bytes>  <pad to 4>
```

and referenced by the address of the byte data (header at −8).
The binary DFM (TPF0) resource uses standard Delphi streaming with
`vaUTF8String`-style items (`0x14 <u32 len> <utf-8>` observed) — both facts
were established empirically and validated by full-file parses.

## 6. UI inventory (TForm1, 804 components)

464 TLabel, 134 TButton, 128 TPanel, 98 TMenuItem, 35 TEdit, 22 TListBox,
18 TRadioButton, 13 TGroupBox, 12 TComboBoxEx, 12 TCheckBox, 11
TDirectoryListBox, 8 TProgressBar, 5 TFileListBox, 4 TPopupMenu, 2 TTimer,
2 TImage, TMainMenu, TFilterComboBox — the DFM dumps live in
`docs/dfm/` for GUI cloning reference.

## 7. Cross-validation

The derived on-disk formats match the publicly documented USBExtreme /
USBAdvance layout consumed by Open PS2 Loader (ul.cfg 64-byte records,
`ul.<id>.<NN>` chunk files, FAT32 requirement, 1 GiB parts). The CRC-32
variant used for the game id (poly 0x04C11DB7, MSB-first, init 0, no final
xor, input = `Trim(name) + #0`) is non-standard and was derived directly
from the disassembly of `0x398738` / `0x398784`.

## 8. Analysis toolkit

The scripts that produced this analysis are shipped in the repository under
[`tools/`](../tools/README.md) — PE resource/DFM extraction, the binary
DFM-to-text converter, Delphi string-reference finder, function-window
extractor, the `usbutil.rcn` decoder, plus the minidump parsers used for the
crash forensics in `crash-v0.0.3-startup.md` / `crash-v0.0.4-iso-explorer.md`
and the hostile-ISO regression test. They run on a stock Python 3, so the
whole derivation stays reproducible.

## 9. What was NOT ported and why

* **CD/DVD burning stack** (`SCSIUnit`, `SptiUnit`, `BurnUnit`,
  MicroUDF image builder): out of scope for a USB game manager; modern
  operating systems handle ISO burning natively.
* **Direct CD/DVD ripping** (SPTI raw reads): replaced by "install from
  ISO image", which is the modern workflow.
* **In-game video ripper** ("Videos activate/deactivate/coincide"):
  implemented as a placeholder scanner (`.PSS`/`.PSS` video detection)
  in the core; the original's exact file-offset patching of GSV files
  was partially reconstructed and is documented in
  `docs/formats/video_rip.md`.
* **"View/Edit ul.cfg" raw record editor**: deliberately left out — a
  raw 64-byte-record editor invites corruption; every record field that
  is safe to touch is editable through dedicated flows (rename, fix
  666.66, recover).
* **"Associate with..." shell file associations** (Windows-only):
  a portable tool should not hijack file associations silently.
* **"SHOW orders" list-order display toggle**: superseded by the
  explicit `sort` command (the original's companion "Sorting" button).

The remaining original list-management features were ported late in the
project: **Sorting** ("Sort the list of games in memory and disk" — a
stable, case-insensitive name sort of the ul.cfg records, then saved
back to disk) and **Restore space** ("Rest. Spc." / the Disk Status
INFORMATION panel — scans the `ul.*` game area for files no record
references, reports files-in-games / obsolete / space-not-occupied /
zero-size / free-space, and deletes the obsolete files on confirmation;
video-rip sidecars of live games are protected).

Everything needed to manage USBExtreme-format game libraries (install,
rename, delete, recover, repair, DNAS/IOPRP patch, PS2Load injection,
sort, restore-space) is implemented in this repository.
