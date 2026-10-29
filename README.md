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
| Export installed game back to ISO (`export all` too) | ✅ |
| Recover orphan `ul.*.00` games | ✅ |
| **Video rip — turn in-game videos off / back on (reversible)** | ✅ |
| `usbutil.rcn` recovery database codec | ✅ |
| `usbutil.ljs` game compatibility database | ✅ |
| DNAS/IOPRP package (`usbutil.dna` + `.dni`) listing & patching | ✅ |
| USB drive discovery (Windows / Linux / macOS) | ✅ |
| CLI (`usbutils`) and Qt Widgets GUI (`usbutils-gui`) | ✅ |

Not ported from the original: the ASPI/SPTI CD/DVD ripping + burning stack
and the MicroUDF bootable-image builder (modern OSes burn ISOs natively);
PS2Load injection is documented in `docs/formats/video_rip.md` but not
implemented yet. The video rip is fully implemented — see
[`docs/formats/video_rip.md`](docs/formats/video_rip.md).

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
usbutils export all      -d /media/usb0 -o /tmp/backups/   # every game
usbutils rename 0        -d /media/usb0 -n "New Name"
usbutils remove 0        -d /media/usb0
usbutils recover         -d /media/usb0 --apply
usbutils videos 0        -d /media/usb0      # list in-game video files
usbutils rip 0           -d /media/usb0      # turn videos off (reversible)
usbutils rip 0           -d /media/usb0 --only MOVIES\\INTRO.PSS
usbutils rip 0           -d /media/usb0 --no-backup   # destructive, like 2009
usbutils unrip 0         -d /media/usb0      # turn videos back on
usbutils id              -n "Game Name"      # show the computed ul id
usbutils dnas list       -p usbutil.dni
usbutils dnas patch 0    -d /media/usb0 -p usbutil.dni --dna usbutil.dna -v 300
usbutils --version
```

The GUI covers the same ground without a terminal: drive picker, game
list (context menu, F2 rename, Del remove), install/export/rename/remove,
structure check, recovery, the video-rip dialog and the DNAS patch dialog —
all long operations run on worker threads with cancelable progress bars.

## Repository layout

```
docs/            reverse-engineering notes + format specifications
src/core/        portable engine library (no external deps)
src/cli/         command-line tool
src/gui/         Qt Widgets application
tests/           core unit + end-to-end tests
data/            game database and assets shipped with the original tool
```

Key engine pieces: `src/core/gamestream.cpp` (random-access view over a
game's chunk files), `src/core/iso9660.cpp` (`walkIso` directory walker
shared by serial extraction, the video ripper and the DNAS locator) and
`src/core/videorip.cpp` (rip/restore with the `.rip` sidecar backup).

## License

MIT — see [LICENSE](LICENSE). Original tool by ISEKO; Sony trademarks
belong to Sony; use with games you own.
