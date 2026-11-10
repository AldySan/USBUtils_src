<div align="center">

<img src="res/icons/usbutils_256.png" width="140" alt="USBUtils icon" />

# USBUtils

**A friendly, open-source manager for PlayStation 2 games on USB drives.**

A modern, cross-platform remake of USBUtil v2.2, the classic
USBExtreme / USBAdvance game manager by ISEKO (2009).

[![Release](https://img.shields.io/github/v/release/AldySan/USBUtils_src)](https://github.com/AldySan/USBUtils_src/releases)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platforms](https://img.shields.io/badge/platforms-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)](#building-from-source)
[![Standard](https://img.shields.io/badge/standard-C%2B%2B17-blue)](#building-from-source)
[![GUI](https://img.shields.io/badge/GUI-Qt%205%20%2F%20Qt%206-green)](#building-from-source)
[![Stars](https://img.shields.io/github/stars/AldySan/USBUtils_src)](https://github.com/AldySan/USBUtils_src/stargazers)
[![Issues](https://img.shields.io/github/issues/AldySan/USBUtils_src)](https://github.com/AldySan/USBUtils_src/issues)

</div>

## About

The original USBUtil put PlayStation 2 games on a USB drive in the format
that USBExtreme and USBAdvance understand. Millions of PS2 fans used it,
then its source code was lost. This project rebuilt it from scratch by
studying the shipped 2009 binary: same on-disk formats, same features, now
as a clean and tested codebase that builds on modern machines.

It comes in two shapes that share one engine:

- **usbutils-gui**, a windowed app for people who would rather not open a
  terminal.
- **usbutils**, a command line tool for scripting and SSH sessions.

The whole reconstruction is written down, in case you are curious:

- How the original binary was taken apart: [docs/REVERSE_ENGINEERING.md](docs/REVERSE_ENGINEERING.md)
- What the files on your drive actually look like: [docs/formats/](docs/formats/)
- How the app is meant to look and feel: [DESIGN.md](DESIGN.md)

## Download

Ready-made packages are on the [releases page](https://github.com/AldySan/USBUtils_src/releases):

- **Windows** (zip): GUI and CLI, with every runtime DLL included. Unzip and
  double-click `usbutils-gui.exe`.
- **Linux** (tar.gz): GUI and CLI, with a bundled Qt runtime. Unpack and run;
  no Qt installation needed.

Both packages carry the complete original data set (DNAS packages, IOPRP
images, PS2LOAD.ELF, game notes), checksum-verified.

The app also looks for updates on its own: the GUI at startup and from the
Help menu, the CLI with `usbutils check-update`.

## Features

### Everyday tasks

- Install a game from an ISO file, or straight from a CD or DVD disc
- List, rename, remove, and sort the games on a drive
- Copy or move games to another USB drive
- Save games back to ISO files, one at a time or several in a batch

### Drive care

- Structure check: missing chunk files, size accounting
- Fix broken serial codes (the "code 666.66" problem) by re-reading each
  game's SYSTEM.CNF
- Recover games that dropped out of ul.cfg
- Clean up drive space by finding leftover files
- Drive report: a text summary of the drive, its games, and anything broken

### Advanced tools

- Turn in-game videos off and back on. Fully reversible, with an automatic
  backup of the original video data.
- Turn in-game audio off and back on: music, voice, and sound effects
  (`.VAG`, `.ADS`, `.SS2`, and friends), with the same reversible backup.
- Find a file inside every game at once, with wildcards and a size filter
- Browse one game's files, extract or replace them
- Open any plain ISO, explore it, add / delete / rename files, edit the
  volume id, and save a modified copy
- DNAS patching from the original package files
- PS2Load injection and removal, reversible

### Under the hood

- Exact original on-disk formats: ul.cfg with 64-byte records, 1 GiB ul.*
  chunk files, the original CRC-32 game id
- One shared engine behind both the CLI and the GUI
- Core library with no external dependencies; the GUI is the only part that
  needs Qt
- USB drive discovery on Windows, Linux, and macOS
- A support-files strip (the original's "Useful acquired" panel) that shows
  at a glance whether the shipped data files are present and healthy

Not ported from the original: the 2009-era disc ripping and burning stack
and the MicroUDF bootable-image builder. Modern systems burn ISO images
natively, and installing straight from a disc is supported instead.

## Building from source

You need:

- A C++17 compiler and CMake 3.16 or newer
- Qt 5 or Qt 6 Widgets for the GUI (build without it using
  `-DUSBUTILS_BUILD_GUI=OFF`)

```bash
git clone https://github.com/AldySan/USBUtils_src.git
cd USBUtils_src
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build    # run the core test suite
```

The test suite uses plain assertions and covers the reverse-engineered
formats end to end, from the CRC-32 game id to a full install and export
round-trip on a generated ISO.

## Using the GUI

The GUI covers everything the CLI does, without a terminal:

- A menu bar (File, Games, Tools, Advanced, Help) plus grouped buttons,
  each with a tooltip that says what it does
- A welcome guide for first-time users, and Quick help from the Help menu
- A drive picker and a game list with a context menu; hold Ctrl or Shift to
  mark several games, Select all and Invert flip the marks
- Keyboard shortcuts: Ctrl+O open, F5 refresh, Ctrl+W close the list,
  Ctrl+I open an ISO, Ctrl+F6 view the configuration, F2 rename, Del remove

Long copy jobs (install, export, copy, move, remove) run on worker threads
with progress bars you can cancel. Quick scans (game file lists, video scan,
find-file) show a busy dialog instead, because they finish in moments.

## Using the CLI

The CLI is handy for scripting or for working over SSH. `-d` always points
at your USB drive. A quick tour:

```
usbutils drives                               # list removable drives
usbutils list -d /media/usb0                  # games on the drive
usbutils install game.iso -d /media/usb0 -n "Shadow of the Colossus"
usbutils check -d /media/usb0                 # structure check
usbutils export 0 -d /media/usb0 -o backup.iso
usbutils export all -d /media/usb0 -o /tmp/backups/   # every game
usbutils rename 0 -d /media/usb0 -n "New Name"
usbutils remove 0 -d /media/usb0
usbutils recover -d /media/usb0 --apply
```

Videos, files, and the PS2Load tool:

```
usbutils videos 0 -d /media/usb0              # list in-game video files
usbutils rip 0 -d /media/usb0                 # turn videos off (reversible)
usbutils rip 0 -d /media/usb0 --only "MOVIES\INTRO.PSS"
usbutils rip 0 -d /media/usb0 --no-backup     # destructive, like the 2009 tool
usbutils unrip 0 -d /media/usb0               # turn videos back on
usbutils audio 0 -d /media/usb0               # list in-game audio files
usbutils rip 0 -d /media/usb0 --audio         # turn audio off (reversible)
usbutils unrip 0 -d /media/usb0 --audio       # turn audio back on
usbutils ps2load 0 -d /media/usb0             # PS2Load status
usbutils ps2load inject 0 -d /media/usb0 --elf data/PS2LOAD.ELF
usbutils ps2load remove 0 -d /media/usb0      # undo the injection
usbutils find "*.pss" -d /media/usb0          # search inside every game
usbutils find "*" -d /media/usb0 --size 33554432
usbutils files 0 -d /media/usb0               # one game's file tree
usbutils extract 0 --path "MOVIES\INTRO.PSS" -d /media/usb0 -o /tmp/
usbutils inject 0 --path SYSTEM.CNF --file new.cnf -d /media/usb0
usbutils copy 0 --to /media/usb1 -d /media/usb0
usbutils move all --to /media/usb1 -d /media/usb0
```

Housekeeping:

```
usbutils report -d /media/usb0 -o report.txt  # drive report
usbutils sort -d /media/usb0                  # sort the game list A to Z
usbutils clean -d /media/usb0                 # find leftover files (dry run)
usbutils clean -d /media/usb0 --apply         # ...and delete them
usbutils fix -d /media/usb0 --apply           # correct code 666.66
```

ISO images, discs, and data files:

```
usbutils iso info game.iso                    # image summary (serial, sizes)
usbutils iso list game.iso                    # file tree of a plain ISO
usbutils iso extract game.iso --path "*.CNF" -o /tmp/
usbutils install /dev/sr0 -d /media/usb0 -n "From Disc"   # from a CD/DVD
usbutils dnas list -p usbutil.dni
usbutils dnas patch 0 -d /media/usb0 -p usbutil.dni --dna usbutil.dna -v 300
usbutils id -n "Game Name"                    # show the computed ul id
usbutils support                              # check the shipped data files
usbutils check-update                         # look for a newer release
usbutils --version
```

On Windows, a drive letter like `E:\` works everywhere `/media/usb0` appears
above, and installing from a disc takes the drive letter alone
(`usbutils install D: -d E:\ -n "From Disc"`).

## Data files

A few tools read the original data files: PS2LOAD.ELF, usbutil.dni and
usbutil.dna (DNAS packages), usbutil.ljs (game compatibility notes),
usbutil.rcn (recovery notes), and the SCEE_DD IOPRP images.

The app looks for them in the folder named by the `USBUTILS_DATA` environment
variable, then next to the program and in a nearby data folder. The release
packages ship them in the right place already. `usbutils support` (or the
support-files strip in the GUI) shows which files were found and where.

## Project structure

```
src/core/    the engine: formats, install/export, all tools (portable, no dependencies)
src/cli/     the usbutils command line tool
src/gui/     the usbutils-gui windowed app (Qt Widgets)
tests/       core unit and end-to-end tests
data/        the original tool's data files, checksum-verified
docs/        reverse-engineering notes and format specifications
res/         icons, the Windows exe icon resource, the Linux desktop entry
tools/       Python analysis toolkit (see tools/README.md)
scripts/     release packaging
```

## Contributing

Bug reports, feature ideas, and pull requests are all welcome. Start with a
fresh [issue](https://github.com/AldySan/USBUtils_src/issues), or read
[CONTRIBUTING.md](CONTRIBUTING.md) if you want to pitch in with code. Please
keep the [Code of Conduct](CODE_OF_CONDUCT.md) in mind; it is short.

## Support development

USBUtils is a free, open-source project built and maintained in spare time.

If the tool helped you out, there are a few easy ways to say thanks:

- **Give the repository a star** on GitHub. It takes one click and helps
  other PS2 fans find the tool.
- **Share it** with anyone who might still be keeping their PlayStation 2
  alive.
- **Buy the maintainer a coffee** on Ko-fi:
  [ko-fi.com/aldysan](https://ko-fi.com/aldysan). Donations fund nothing
  glamorous, just coffee and late-night coding time.

All of it is optional. The tool is free, and it stays free.

## Credits

- The original **USBUtil v2.2 rev1.0 Ultimate** was written by Israel Ravelo
  Hernández (ISEKO) and released in 2009. Its design and on-disk formats are
  the foundation of this project; all credit for the original tool belongs
  to ISEKO.
- Sony, PlayStation, and PS2 are trademarks of Sony Interactive
  Entertainment. This project is not affiliated with or endorsed by Sony.
  Please use it with games you own.

## License

[MIT](LICENSE)
