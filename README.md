# USBUtils (src)

An open, cross-platform **C++17 / Qt** reimplementation of **USBUtil v2.2
rev1.0** — the classic PlayStation 2 *USBExtreme / USBAdvance* game manager
originally written in Delphi by **ISEKO (2009)**. The original source code
was lost; this project was produced by **reverse engineering the shipped
binary** and is maintained as a modern, documented, testable codebase.

> Reverse-engineering notes: [`docs/REVERSE_ENGINEERING.md`](docs/REVERSE_ENGINEERING.md)
> On-disk formats: [`docs/formats/`](docs/formats/)

## Features

| Feature | Status |
|---|---|
| Read/write `ul.cfg` (64-byte records) | ✅ |
| Install game from ISO (1 GiB `ul.<id>.<serial>.NN` chunks) | ✅ |
| Serial extraction from ISO `SYSTEM.CNF` | ✅ |
| Game id = non-standard CRC-32 of the name (as the original) | ✅ |
| Rename / remove games (renames chunk files) | ✅ |
| Structure check (missing parts, size accounting) | ✅ |
| Export installed game back to ISO | ✅ |
| Recover orphan `ul.*.00` games | ✅ |
| `usbutil.rcn` recovery database codec | ✅ |
| `usbutil.ljs` game compatibility database | ✅ |
| DNAS/IOPRP package (`usbutil.dna` + `.dni`) listing & patching | ✅ |
| USB drive discovery (Windows / Linux / macOS) | ✅ |
| CLI (`usbutils`) and Qt Widgets GUI (`usbutils-gui`) | ✅ |

Not ported from the original: the ASPI/SPTI CD/DVD ripping + burning stack
and the MicroUDF bootable-image builder (modern OSes burn ISOs natively);
video ripping is documented in `docs/formats/video_rip.md`.

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build            # run the core test suite
```

* CLI needs nothing but a C++17 compiler.
* GUI needs Qt 5 or Qt 6 Widgets — build without it using
  `-DUSBUTILS_BUILD_GUI=OFF`.
* Tests use plain assertions, no framework.

## CLI usage

```
usbutils drives                              # list removable drives
usbutils list            -d /media/usb0      # list games on a drive
usbutils install game.iso -d /media/usb0 -n "Shadow of the Colossus"
usbutils check           -d /media/usb0      # structure check
usbutils export 0        -d /media/usb0 -o backup.iso
usbutils rename 0        -d /media/usb0 -n "New Name"
usbutils remove 0        -d /media/usb0
usbutils recover         -d /media/usb0 --apply
usbutils id              -n "Game Name"      # show the computed ul id
usbutils dnas list       -p usbutil.dni
usbutils dnas patch 0    -d /media/usb0 -p usbutil.dni --dna usbutil.dna -v 300
```

## Repository layout

```
docs/            reverse-engineering notes + format specifications
src/core/        portable engine library (no external deps)
src/cli/         command-line tool
src/gui/         Qt Widgets application
tests/           core unit + end-to-end tests
data/            game database and assets shipped with the original tool
```

## License

MIT — see [LICENSE](LICENSE). Original tool by ISEKO; Sony trademarks
belong to Sony; use with games you own.
