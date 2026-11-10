# Video rip / audio rip / PS2Load / misc formats

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

Little-endian, `#pragma pack(1)`. Current builds write **USRRIP03**
(block-compressed payload + per-entry CRC32); **USRRIP02** and
**USRRIP01** sidecars from older builds are still read (and upgraded
to v3 the next time more media are ripped with a backup):

| Offset | Size | Field |
|--------|------|-------|
| 0 | 8 | magic `"USRRIP01"` / `"USRRIP02"` / `"USRRIP03"` |
| 8 | 4 | uint32 entry count |
| 12 | 8 | uint64 uncompressed payload size (v2/v3 only) |
| 12 | 16×N | entries (v1/v2): `{ uint64 streamOffset, uint64 size }` |
| 20 | 20×N | entries (v3): `{ uint64 streamOffset, uint64 size, uint32 crc32 }` |
| … | … | payload (below) |

The payload is the original bytes of each entry, in entry order. In a
v1 sidecar it is stored verbatim. In v2/v3 it is a sequence of 1 MiB
blocks, each self-describing (see `core/lzpack.h`):

```
uint32 hdr    bit 31 = 1 -> LZSS data (uint32 compLen follows),
              bit 31 = 0 -> stored data; bits 0-30 = raw block length
data          compLen LZSS bytes | rawLen verbatim bytes
```

The LZSS micro-format is byte-oriented: a control byte precedes up to 8
units (1 bit each, 1 = literal byte, 0 = 3-byte match token: uint16
back-offset 1..65535 within the same block + uint8 length−4). The
decoder rejects any offset reaching before the block start, so a
corrupt sidecar fails loudly instead of writing garbage into the game.

The v3 `crc32` of an entry is standard CRC-32/ISO-HDLC
(`usbutils::crc32` in `core/crc32.h`, zlib-compatible: polynomial
0xEDB88320 reflected, init and xorout 0xFFFFFFFF — `crc32(0, "123456789", 9)`
is `0xCBF43926`) computed over the entry's own 16 record bytes —
little-endian `streamOffset`, then `size` — followed by the entry's
uncompressed payload bytes. Folding the record into the checksum means
a corrupted index (a flipped offset bit passes every structural check,
since the size sum still matches the payload size) fails exactly like
corrupted payload data would.

Restore verifies every entry's CRC32 **before writing a single byte
into the game**: a corrupt backup is reported while the media are still
blanked (a blanked game still boots; a half-restored one may not), the
failing file is named when the index is intact, and the sidecar is kept
for recovery from a healthy copy. The verification pass is read-only —
one extra read of the sidecar — and reuses the progress callback, so a
progress indicator runs to 100% twice (verify, then write). v1/v2
backups carry no checksums: they pass the structural checks (magic,
count clamp, payload size) and are restored as-is; ripping more media
with a backup upgrades them to v3, which is the cheapest way to make
an old backup verifiable.

Ripping more media later appends entries and re-encodes the whole
payload — previously backed-up bytes are re-read from the old sidecar
(the game stream already holds zeros for them). An incremental
destructive rip never damages an existing backup.

What to expect: PS2 audio (`.VAG`, `.VB`, `.MIB` …) is raw 4-bit ADPCM
with long zero runs, so `.arip` backups typically shrink dramatically
(synthetic ADPCM-like patterns measure ~3–60% of the original, depending
on how much silence they hold). `.PSS` video is already MPEG-2
entropy-coded, so `.rip` backups stay close to their raw size — a block
that cannot shrink is stored verbatim at ~4 bytes per MiB of overhead
instead of growing.

Why not zlib: the core library is deliberately dependency-free, and
zlib would gain almost nothing on the MPEG-2 half of the payload
anyway.

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

## In-game audio handling (audio rip)

A USBUtils extension; the original tool had no audio rip. The same
blank/restore mechanism, applied to standalone audio files:

* Detected extensions (case-insensitive): `.VAG` (Sony ADPCM),
  `.VAB`/`.VH`/`.VB` (VAB sample banks), `.ADS`/`.ASS`/`.SS2` (ADPCM
  streams), `.MIB`/`.MIH` (interleaved music and its header).
* The backup sidecar is `ul.<ID>.<SERIAL>.00.arip` — the same container
  as `.rip` (USRRIP03, block-compressed, per-entry CRC32), so a video rip
  and an audio rip on the same game keep independent backups and can be
  restored in either order.
* Video detection (`.PSS`) and audio detection never overlap, so
  `unrip` and `unrip --audio` each restore exactly their own media.
* Like the video rip it is size-preserving: files are blanked with
  zeros in place, so the ISO byte count, every LBA and the drive usage
  stay exactly the same (the USBExtreme loader expects a byte-exact
  image). What you gain is a silent game, not free space.
* Drive-wide features know about the sidecar: "Clean up drive space"
  counts it as a game file (never deletes it), and copying or moving a
  game carries it along.

### CLI

```
usbutils audio <index>  -d <dir>                        # list + state
usbutils rip <index>    -d <dir> --audio [--only path,...] [--no-backup]
usbutils unrip <index>  -d <dir> --audio                # restore + drop backup
```

### GUI

`Advanced > Audio (turn off / restore)...` opens the same per-game dialog
as the video rip, listing audio files instead.

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
