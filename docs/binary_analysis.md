# Binary analysis — analyzed specimen

The reverse engineering documented in this repository was performed on the
following files (SHA-256):

```
b89175f78ac89288cd4dcd0d9cfeb1f9e9a9a255571b8447c9d9814fa1a571b7  USBUtil v2.2 rev1.0_English.exe
bc26534208b5f8f21fc7838ccba9f1649fe506172e09e95fb4430a90509c1dfa  USBUtil v2.2 rev1.0_Spanish.exe
50a418bdd965889bd67bc8ca68c64900e2bbe88f3930c51af61ef6c51cff3e46  PS2LOAD.ELF
92c6ec278340f220ac8637ce23e4d2932f6f0be191f91956cde1beff3ea00830  usbutil.aso
5f9101608719fb3f7c480b4357ea95669c47c24c9d2cee372e5df8b5922b1351  usbutil.dna
69b85962ec09526f5b2f7e6c54e8bc4bd216fc059e6cf294b9122e0a6eac652c  usbutil.dni
2110c7225587ba95b0270dd32c1dfb1f862399507ed367a9ef84d889d3da747b  usbutil.ljs
fe3d284df4f9419067a2256c5bacb4cda896fad9c30eaccfdee352382d0d028d  usbutil.rcn
520d74545b4e24dc25ab0d01c660da271c199080d0834b38a20114caeadc991d  SCEE_DD/IOPRP16.IMG
9a3f9d337a23c71ade451e95f7fd03e038f4cc4b21d47b3032c57591c83a4557  SCEE_DD/IOPRP165.IMG
8613460cac02cf87a65f23446b86a04a9072a1faca796b05e1d67b910a31316a  SCEE_DD/IOPRP20.IMG
e4857da724efb48213d4cb805637b461846767e4dd659905c32d9a007f5f90cc  SCEE_DD/IOPRP202.IMG
```

Distributed as `USBUTIL_v2.2_rev1.0_EN (English).7z`
(827,384 bytes, LZMA2+LZMA+BCJ2).

## Method

1. **Static PE analysis** — section table, import table, Delphi
   identification (section names `CODE/DATA/BSS`, `SOFTWARE\Borland\Delphi\RTL`
   strings, PACKAGEINFO unit list, DVCLAL checksum resource).
2. **Resource extraction** — custom PE resource-tree parser pulled
   `TForm1` (364,557 B), `TBurnerForm`, `TBurnForm`, `TISOForm`, `TForm2`
   binary DFMs; a custom TPF0 stream decoder (validated by full-file parse)
   produced the text DFM dumps in `docs/dfm/`.
3. **Full disassembly** — GNU objdump over the 0xBC69C-byte `CODE`
   section (279,276 instructions); Delphi string constants located by their
   `FFFFFFFF <u32 len> <data>` pool layout and cross-referenced by address.
4. **Data-format derivation** — every on-disk format was derived from the
   code that reads/writes it (addresses cited throughout the docs), then
   validated empirically against the shipped `usbutil.rcn`, `usbutil.dna`,
   `usbutil.dni` and `usbutil.ljs` files.
5. **Independent verification** — the CRC-32 variant used for game ids was
   re-implemented independently (Python) and matched the C++ port on all
   probe vectors; the `rcn` codec decodes 257 entries from the shipped file.

## Tools used

`objdump` (GNU binutils 2.44, pei-i386), Python 3.12 analysis scripts
(kept in the project that produced this repo), `7-Zip 25.01`, GCC 14.2,
CMake 3.31, Qt 6.8.
