# USBExtreme chunk file naming

**Derived from:** `SplitISO @0x3A112F`, `CreateGameRecord @0x39DCB0`,
`0x399E48` (base-name builder), chunk-suffix concat at `0x3A1308..0x3A1340`,
`ScanULPattern @0x3A2B77` (`FindFirst "ul.*.00"`), `ComputeGameId @0x398784`.

A game occupies consecutive chunk files in the **root of the USB drive**:

```
ul.<IDHEX>.<SERIAL>.NN
```

* `IDHEX` — 8 uppercase hex digits: `CRC32(Trim(name) + NUL)` using
  polynomial `0x04C11DB7` MSB-first, init `0`, no final xor.
  (Non-standard CRC — table generator reconstructed at `0x398738`.)
* `SERIAL` — the 11-char disc serial extracted from `SYSTEM.CNF`
  (`BOOT2 = cdrom0:\SLUS_202.65;1` → `SLUS_202.65`), the `;1` ISO9660
  version suffix and any trailing `.1` are trimmed; padded with `0` to a
  canonical 11 chars.
* `NN` — chunk index, two decimal digits (`00`, `01`, …). The original
  writes `".0" + Chr('0'+i)`, i.e. it only supports single-digit indexes
  (up to 10 parts = 10 GiB); this reimplementation zero-pads two digits,
  which stays compatible for i ≤ 9.

Example — game "Shadow of the Colossus" (serial `SCUS_97472`):

```
ul.2D9E3F51.SCUS_97472.00    (1 GiB)
ul.2D9E3F51.SCUS_97472.01    (remaining bytes)
```

## Chunking rules

* Each chunk holds at most **1 GiB** (`0x40000000` bytes).
* `parts = ceil(total_size / 1 GiB)` (division + modulo at `0x3A11C2`).
* The last chunk holds the remainder.
* Chunk files are written with an 8 MiB buffered copy loop with progress
  callback and cancel flag (`WriteChunkFile @0x39CF44`).

## Recovery / scanning

Orphan detection globs `ul.*.00` (`FindFirst` attrs `0x5F`) — any first
chunk without a matching `ul.cfg` record is a recovery candidate.

## Size accounting

Total game size = Σ chunk file sizes. The "Correct code 666.66" menu item
fixes `ul.cfg` records whose **serial** field holds the magic broken value
`666.66` (written by some third-party tools): the real serial is re-derived
from the game's own `SYSTEM.CNF` (BOOT2 line) and both the record and the
chunk file names are updated (`ul.<ID>.666.66.NN` -> `ul.<ID>.<serial>.NN`).
The original compares 11 chars of the game-list code column against the
marker string at 0x39B854; "No se pudo renombrar codigo 666.66" is the
failure path when the serial cannot be re-derived.
