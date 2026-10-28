# Video rip / PS2Load / misc formats

## In-game video handling ("Videos activate/deactivate/coincide")

PS2 DVD games store video as `.PSS` (MPEG-2 program streams) and `.PSS`-like
`.PAK`/`.DAT` files. USBUtil's ripper:

* **Deactivate** — replaces video file content with a tiny stub
  (scan-and-blank), preserving file size and therefore LBA offsets.
* **Activate** — restores from the original ISO.
* **Coincide** — only blanks videos that match a name/size list.

Detection string refs: `'.PSS'`, `'.PSS.'` constants in the CODE pool and
`No existen formatos de video reconocidos por esta versión.` ("no
recognized video formats") @0x940B8.

This reimplementation (`core/videorip.cpp`) performs size-preserving
blanking of `.PSS`/`.PSS`-detected streams inside chunk files with an
undo sidecar (`.usbutils-rip` storing original byte ranges), which is a
safer, reversible equivalent of the original's destructive rip.

## PS2LOAD.ELF injection

`PS2LOAD.ELF` (218,252 bytes, MIPS ELF, shipped with the tool) is the
boot loader used to build bootable media. The original writes it into the
ISO9660/MicroUDF image it constructs via its burning stack. The injection
point selected in the UI ("PS2Load options") determines whether the ELF
replaces the game's primary volume descriptor boot record or is added as
`PS2LOAD.ELF;1` in the root directory.

This repo re-implements the second form (add file) on the ISO level via
`core/ps2load.cpp` (rebuilds the ISO directory with the extra file,
adjusting LBA allocation) and keeps the original ELF in `data/`.

## IOPRP images (SCEE_DD)

`IOPRP16.IMG` … `IOPRP202.IMG` shipped in `SCEE_DD/` are Sony IOP ROM
replacement images; see `formats/dnas_package.md`. All begin with the
ASCII magic `RESET`.

## usbutil.aso — tool config

```
//USBUtil 2.00 by ISEKO 2009
DIR=<last used directory>
REP=; param=; del=1
EDIT=; param=; del=0
OTRO=; param=; del=0
```

Key/value INI-like text file; `del=1` marks entries removable. The
reimplementation uses `usbutils.ini` with the same semantics.
