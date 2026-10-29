# Video rip / PS2Load / misc formats

## In-game video handling ("Videos activate/deactivate/coincide")

PS2 DVD games store video as `.PSS` (MPEG-2 program streams) and
`.PSS`-like multi-part containers (`MOVIE.PSS.01`, detected by the
original's `.PSS.` constant). USBUtil's ripper:

* **Deactivate** — blanks the video file's bytes, preserving file size and
  therefore LBA offsets (the USBExtreme loader expects a byte-exact ISO
  image, so sizes must not change).
* **Activate** — restores the original bytes.
* **Coincide** — only blanks videos that match a name/size list.

Detection string refs: `'.PSS'`, `'.PSS.'` constants in the CODE pool and
`No existen formatos de video reconocidos por esta versión.` ("no
recognized video formats") @0x940B8.

### This reimplementation (`core/videorip.cpp`)

* **Scan** — walks the game's ISO9660 directory tree through
  `GameStream` (`core/gamestream.cpp`, random access over the
  `ul.<ID>.<SERIAL>.NN` chunks) and lists every `.PSS`-named file with
  its exact LBA-derived offset and size.
* **Rip (turn off)** — size-preserving zeroing of the selected ranges.
  Before any byte is touched, the originals are streamed into a sidecar
  backup (the original's destructive rip is available in the CLI via
  `--no-backup`).
* **Restore (turn on)** — writes the backed-up ranges back and removes
  the sidecar. Games export bit-identical to the source ISO afterwards.

### Sidecar backup format — `ul.<ID>.<SERIAL>.00.rip`

Little-endian, `#pragma pack(1)`:

| Offset | Size | Field |
|--------|------|-------|
| 0 | 8 | magic `"USRRIP01"` |
| 8 | 4 | uint32 entry count |
| 12 | 16×N | entries: `{ uint64 streamOffset, uint64 size }` |
| 12+16N | … | original bytes of each entry, in entry order |

Ripping more videos later appends entries; previously backed-up data is
copied verbatim (the game stream already holds zeros for it — re-reading
it would destroy the backup). An incremental destructive rip never
damages an existing backup.

### CLI

```
usbutils videos <index>  -d <dir>                 # list + state
usbutils rip <index>     -d <dir> [--all | --only path,...] [--no-backup]
usbutils unrip <index>   -d <dir>                 # restore all + drop backup
```

### GUI

`Videos...` opens a per-game dialog (table, sizes, current state) with
"Turn OFF selected videos" / "Turn all videos back ON" — both run on a
worker thread behind a cancelable progress dialog. The GUI always makes
a backup.

## PS2LOAD.ELF injection

Moved to its own document — see [`ps2load.md`](ps2load.md) for the
full reconstruction of the "PS2Load injector v1.2" (marker record,
MSYSTEM.CNF backup, PS2L_ name mangling, in-place SYSTEM.CNF rewrite,
removal). Implemented by `core/ps2load.cpp`.

## IOPRP images (SCEE_DD)

`IOPRP16.IMG` … `IOPRP202.IMG` shipped in `SCEE_DD/` are Sony IOP ROM
replacement images; see `formats/dnas_package.md`. All begin with the
ASCII magic `RESET`. DNAS patching locates the game's `IOPRP*.IMG` ISO
directory entry for the exact extent — never a blind magic scan — and
falls back to a bounded `RESET\0\0\0` scan only for non-standard rips.

## usbutil.aso — tool config

```
//USBUtil 2.00 by ISEKO 2009
DIR=<last used directory>
REP=; param=; del=1
EDIT=; param=; del=0
OTRO=; param=; del=0
```

Key/value INI-like text file; `del=1` marks entries removable. The
reimplementation uses Qt `QSettings` (organization `AldySan`, app
`USBUtils`) with the same semantics.
