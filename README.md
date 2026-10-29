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
| **PS2Load injection / removal** (marker record, MSYSTEM.CNF backup, reversible) | ✅ |
| **Find file in games** (wildcard + size search across every game) | ✅ |
| **Copy / move games to another drive** (chunk stream + record, sidecar carried) | ✅ |
| **General report** (drive, games, structure evaluation, broken codes) | ✅ |
| **Correct code 666.66** (fix broken serials, re-derived from SYSTEM.CNF) | ✅ |
| **Sort games A–Z** (original "Sorting" — reorders ul.cfg on disk) | ✅ |
| **Restore space** (original "Rest. Spc." — find & delete left-over `ul.*` files) | ✅ |
| **Per-game file explore / extract / inject** (same-size in-place replace) | ✅ |
| **Open ISO to [Explore/Modify]** (original TISOForm — tree view, add/delete/rename files, editable volume id, save a modified copy) | ✅ |
| **Create game from CD/DVD** (raw-device install: GUI disc picker with PS2 probing, `install /dev/sr0` on the CLI) | ✅ |
| **Select / Invert selection** + batch backup-export and batch remove of marked games | ✅ |
| **Close ul.cfg** / **View current config** (original File/Tools menu entries) | ✅ |
| **"Useful acquired"** panel — live status of the shipped support files (`usbutils support` on the CLI) | ✅ |
| Complete original data set: 5 × `usbutil.*` + `SCEE_DD/IOPRP*.IMG` + `PS2LOAD.ELF`, checksum-verified | ✅ |
| Application icon on Windows (embedded exe icon + version info) and Linux (hicolor + `.desktop`) | ✅ |
| USB drive discovery (Windows / Linux / macOS) | ✅ |
| CLI (`usbutils`) and Qt Widgets GUI (`usbutils-gui`) | ✅ |

Not ported from the original: the ASPI/SPTI CD/DVD ripping + burning stack
and the MicroUDF bootable-image builder (modern OSes burn ISOs natively).
Installing straight from a disc is supported instead: the GUI's
"Install a game from a CD/DVD disc" probes each drive and reads the disc
raw; on the CLI pass the device (`usbutils install /dev/sr0 ...`, or a
Windows drive like `D:`).
The video rip is fully implemented — see
[`docs/formats/video_rip.md`](docs/formats/video_rip.md); PS2Load injection
is documented in [`docs/formats/ps2load.md`](docs/formats/ps2load.md).

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
usbutils ps2load 0       -d /media/usb0      # PS2Load status
usbutils ps2load inject 0 -d /media/usb0 --elf data/PS2LOAD.ELF
usbutils ps2load remove 0 -d /media/usb0      # undo the injection
usbutils id              -n "Game Name"      # show the computed ul id
usbutils dnas list       -p usbutil.dni
usbutils dnas patch 0    -d /media/usb0 -p usbutil.dni --dna usbutil.dna -v 300
usbutils find "*.pss"    -d /media/usb0      # search files inside every game
usbutils find "*"        -d /media/usb0 --size 33554432
usbutils files 0         -d /media/usb0      # list one game's file tree
usbutils extract 0 --path MOVIES\\INTRO.PSS -d /media/usb0 -o /tmp/
usbutils inject 0 --path SYSTEM.CNF --file new.cnf -d /media/usb0
usbutils copy 0 --to /media/usb1 -d /media/usb0   # copy game to 2nd drive
usbutils move all --to /media/usb1 -d /media/usb0 # move every game over
usbutils report          -d /media/usb0 -o report.txt
usbutils sort            -d /media/usb0      # sort the game list A-Z
usbutils clean           -d /media/usb0      # find left-over files (dry run)
usbutils clean           -d /media/usb0 --apply  # ...and delete them
usbutils fix             -d /media/usb0 --apply  # correct code 666.66
usbutils iso info game.iso                   # image summary (serial, sizes)
usbutils iso list game.iso                   # file tree of a plain ISO
usbutils iso extract game.iso --path "*.CNF" -o /tmp/
usbutils install /dev/sr0 -d /media/usb0 -n "From Disc"   # CD/DVD device
usbutils support                              # "Useful acquired": check the
                                              # shipped data files
usbutils check-update                               # look for a newer release
usbutils --version
```

Both the CLI and the GUI can check for new releases on their own
(`usbutils check-update`; the GUI checks automatically at startup and
shows a dialog when something newer is available).

The GUI covers the same ground without a terminal, built for non-technical
users: a menu bar, plain-English grouped buttons with tooltips, a welcome
guide, drive picker, game list (context menu, F2 rename, Del remove,
mark several with Ctrl/Shift + Select all / Invert), install (ISO file or
CD/DVD disc), copy/move/rename/remove, batch backup-export of marked
games, find-file, per-game file browser (extract/replace), check & repair
(incl. code 666.66), recovery, drive report, the video-rip, DNAS and
PS2Load dialogs, "Open ISO to [Explore/Modify]", Close ul.cfg, View
current configuration and the "Useful acquired" support-file strip — all
long operations run on worker threads with cancelable progress bars. The
GUI checks for updates automatically at startup and from the Help menu.

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
