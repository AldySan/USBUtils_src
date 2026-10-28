# usbutil.rcn — recovery database

**Derived from:** save path `0x39306B` (encoder loop) plus full-file
empirical validation (193 entries decoded from the shipped file).

`usbutil.rcn` remembers the (serial, name) pairs of previously managed
games so the "Recover games" feature can rebuild `ul.cfg` records for
orphan `ul.*.00` chunk files.

## Stream format

```
entry(i) := <len:1 byte, RAW> <data: len-1 bytes, XOR-encoded>
key       := len XOR listbox_index
data      := serial(11 chars) || game_name(rest)
```

* `len` = length of the *string* + 1 (i.e. `data` is `len-1` bytes).
* The single length byte is stored **unencoded**; every data byte is
  XORed with `key = len ^ index`.
* `index` is the index of the game in the tool's list box — header rows
  (whose chars 12..16 equal `JUEGO`, see compare @0x39310E) are skipped
  when writing but still advance the index, so stored indexes have gaps.
* There is no explicit file header; sections may restart numbering.

## Verified decode of the shipped file

```
[  0] SLUS_202.65 | '007 A_Under Fire'
[  1] SLES_534.81 | '10.000 bullet'
[  2] SCES_533.58 | '24_PREMASTER19_1'
...
[136] SLPS_255.42 | 'Naruto Cronicas'
[137] SLUS_215.75 | 'NARUTO ULTIMATE NINJA 2'
...
```

135 + 58 = 193 entries recovered; the tail of the shipped file (~2.5 KB)
contains additional non-game bookkeeping records.

## Serial alphabet

Serials observed include `SLUS_`, `SLES_`, `SCES_`, `SCUS_`, `SLPS_`,
`PS2L_` — the decoder must accept `[A-Z0-9]{4}_\d{3}\.\d{2}`.

# usbutil.ljs — game compatibility database

Plain Latin-1 text, CRLF, one game per line (1432 lines in v2.2 rev1.0):

```
*007 agente corrupto
*007 desde Rusia con amor
007 Nightfire
187 Ride or Die (cambia CDVDSTM.IRX + PS2LOAD)
+25 To life in U.S.A. dnas300
7 Sins  (sin sonido)
```

Leading `*` marks special entries; parenthesised suffixes carry
compatibility notes: `dnas300` (needs IOPRP 300 patch), `cambia
CDVDSTM.IRX + PS2LOAD`, `sin sonido` (no sound), etc. This repo ports the
list to `data/games.ljs` verbatim and parses the annotations.
