# usbutil.dna / usbutil.dni — DNAS (IOPRP) package

**Derived from:** strings `usbutil.dna`, `usbutil.dni` (refs `0x3971CD`
region), the SCEE_DD folder shipped with the tool, and the `.dni` file's
own header comments (author ISEKO, May 2009).

## Purpose

The PS2 IOP (Input/Output Processor) ROM images (`IOPRP*.IMG`) differ per
console BIOS revision. Some games ("DNAS" protected or incompatible IOP
revisions) run from USB only when the IOPRP image embedded in the game
image is replaced with one matching the console. USBUtil ships a
**concatenated blob** of known-good IOPRP images and can inject the right
one into an installed game's chunk files.

## usbutil.dni — index

Plain-text (CRLF, Latin-1) index:

```
// comment lines start with '//'
DNAS<ver>[<size_in_bytes>];
```

Example (v1.0 package):

```
DNAS16[91909];
DNAS165[98901];
DNAS20[109809];
DNAS202[110993];
DNAS23[230329];
DNAS242[278353];
...
DNAS254[278353];
```

The numeric suffix is the console IOP revision (e.g. `DNAS250` =
BIOS 2.50). Sizes are decimal byte counts.

## usbutil.dna — data blob

Simple concatenation of the `IOPRP*.IMG` files **in the exact order listed
by the index** (the original build recipe, from the file's own comment:

```
copy /b IOPRP16.IMG+IOPRP165.IMG+...IOPRP#.IMG usbutil.dna
```

Each `IOPRP*.IMG` starts with the ASCII magic `RESET` followed by zeros
(RIOC ROM image header), which allows independent verification of blob
boundaries.

The shipped v2.2 rev1.0 package: 5,686,213 bytes, 22 indexed entries
(indexed span 5,373,942 bytes; the blob contains one extra trailing image
beyond the shipped index).

## Patching flow (menu "Patch for DNAS/IOPR/IRX")

1. Read the game's target IOPRP region (the `IOPRP*.IMG` embedded in the
   game image, located by scanning the chunk files for the `RESET` magic).
2. Look up the replacement image inside `usbutil.dna` using `usbutil.dni`
   offsets (running sum of indexed sizes).
3. Overwrite the region **in place** — replacement images must be ≤ the
   original slot (padding with zeros), otherwise the tool refuses.
4. The same mechanism replaces `.IRX` modules (string refs `.IRX` @0x945AC)
   and supports `CDVDSTM.IRX` swaps noted in the game database.
