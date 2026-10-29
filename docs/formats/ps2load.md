# PS2Load injection — `docs/formats/ps2load.md`

**Derived from:** the "PS2Load injector v1.2" panel (TFORM1 DFM:
`IncluirPS2Loader1` / `InyectarPS2Load1` menus, Panel27 with BitBtn1
"Inject" / BitBtn2 "Remove"), the injection engine `0x3BAD08`, the
record scanner `0x3B9D64`, the existence checker `0x3B9FF3` /
`0x3BA1EC`, the PS2L_ name builder `0x3BAB38`, the empty-sector finder
`0x3B9CF0` / `0x3BA4EC` and the status reporter around `0x3BA5B0`.
The tool's about-box credits it as *"PS2Load injector v1.2"* (label
@0x3BA9D8 region), and the 2009 psXtools.de release thread describes
the standalone v1.0: *"The file MSYSTEM.CNF is created automatically by
the injector … There are many games that only work integrating
PS2Load.elf."*

## Why it exists

Some games refuse to boot from USB through USBAdvance/USBExtreme with
their own boot ELF. For those, the community swapped in **PS2LOAD.ELF**
(a ~213 KiB MIPS loader, shipped as `data/PS2LOAD.ELF`) as the boot
program; it then brings up the real game. The ljs database annotates
hundreds of games with `ps2load` for exactly this reason. The injector
performs that swap **without moving any file**: *"the injector
increases PS2L compatibility by protecting 100% of the LBAs"*
(@0xB9EAC).

## What an injection looks like on disk

Three ISO9660 records are appended to the game's **root directory**
(the original required `>= 0x90` bytes of room there, check @0x3BAE1A,
message @0x3BB3CC), and two blobs are appended to the **end of the last
chunk file**:

| record | kind | extent | size | purpose |
|---|---|---|---|---|
| `SCEE_DD` | directory (flags 0x02) | an all-zero sector in `[0, rootDirLba)` (typically 15) | 0x158 | **marker** |
| `MSYSTEM.CNF;1` | file | appended backup | 2048 | SYSTEM.CNF backup |
| `PS2L_<name>;1` | file | appended loader | size of the ELF | the loader itself |

* **Marker record** — the "SCEE_DD" directory record points at an
  all-zero system-area sector *below the root directory's LBA*. That
  placement is what the scanner keys on:
  * no `SCEE_DD` record → not injected;
  * `SCEE_DD` extent `< rootDirLba` → **injected by the tool**
    (*"PS2Load introducido por PS2L_Injector/USBUtil v2.1"*, removable);
  * `SCEE_DD` extent `>= rootDirLba` → **added manually** (UltraISO
    etc., *"PS2Load MANUAL no puede ser borrado"* — this tool refuses
    to remove those).

* **The name mangling** (0x3BAB38) — the boot file name is taken from
  the BOOT2 path, then:
  * `name` ≥ 8 chars → drop the first 5 (the `SCUS_`-style prefix);
  * shorter → keep the first 3;
  * prepend `PS2L_` and re-append the extension:

  ```
  SCUS_973.99  ->  PS2L_973.99.99
  SLPS_250.99  ->  PS2L_250.99.99
  ```

* **SYSTEM.CNF is rewritten in place**: the boot file name inside the
  BOOT2 path is swapped for the mangled record name
  (`BOOT2 = cdrom0:\PS2L_973.99.99;1`). The `PS2L` substring doubles as
  an in-CNF marker — the original greps the SYSTEM.CNF extent for it
  (0x3B9FF3). The write stays inside the CNF's own sector.

* The original **game name in `ul.cfg` and every other LBA is
  untouched**; `parts` does not change.

## Removal

Only for tool-made injections: the SYSTEM.CNF sector is restored from
the `MSYSTEM.CNF` backup, the three records are blanked out, and the
appended bytes are trimmed off the last chunk — the game exports
bit-identical to its pre-injection ISO afterwards.

## This reimplementation (`core/ps2load.cpp`)

CLI:

```
usbutils ps2load <index>        -d <dir>               # status
usbutils ps2load inject <index> -d <dir> [--elf <PS2LOAD.ELF>]
usbutils ps2load remove <index> -d <dir>
```

GUI: *PS2Load…* in the game list context menu / button row.

Divergences from the 2009 tool (deliberate, each verified by tests):

* **`MSYSTEM.CNF` is a full 2048-byte snapshot of the SYSTEM.CNF
  sector.** The original stored a short, oddly assembled string (a
  7-piece concatenation around `"\" + <edit text> + "\cdrom0:" +
  <cnf> + ";1"` whose exact layout could not be fully reconstructed;
  it embedded the *modified* CNF text). A sector snapshot is a strict
  superset: it restores the CNF **bit-exactly** (the longer PS2L_ boot
  name can spill a few bytes past the original CNF length inside its
  sector) and still contains the original `BOOT2` line the original
  tool's "code 666.66" fixer read the serial from.
* **Every appended blob is sector-aligned.** The original appended the
  backup and the loader back-to-back, which could leave the loader's
  record extent pointing at a misaligned start. Here the backup is a
  whole sector, so the loader always begins exactly at `LBA * 2048`.
* **Records carry true ISO9660 lengths** (33 + name, padded even)
  instead of the original's fixed 0x30, whose 48-byte claim could be
  shorter than the actual name+header for long boot names.
* Injection refuses games whose chunk stream does not end on a sector
  boundary (real installs always do), which is what makes removal able
  to restore the exact original file size.

The engine shares the ISO9660 record layout with `core/iso9660.cpp`;
see there for the ECMA-119 field offsets.
