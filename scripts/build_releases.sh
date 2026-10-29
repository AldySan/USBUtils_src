#!/bin/bash
# Build and package USBUtils release artifacts (Linux + Windows) for x86-64.
# - Linux:   usbutils-<ver>-linux-x86_64.tar.gz   (GUI+CLI+Qt runtime closure
#                                                   + icons + .desktop)
# - Windows: usbutils-<ver>-windows-x86_64.zip     (GUI+CLI+Qt/MinGW DLLs,
#                                                   exes carry the icon)
# Data files shipped in data/: the complete original set (checksum-verified
# against docs/binary_analysis.md):
#   PS2LOAD.ELF usbutil.aso usbutil.dna usbutil.dni usbutil.ljs usbutil.rcn
#   SCEE_DD/IOPRP16.IMG IOPRP165.IMG IOPRP20.IMG IOPRP202.IMG
set -euo pipefail

REPO=/home/z/my-project/USBUtils_src
QT_HOST=/home/z/my-project/tools/qt6/usr
QT_WIN=/home/z/my-project/tools/qt6-win/6.8.2/mingw_64
MINGW=/home/z/my-project/tools/mingw-cross/usr/x86_64-w64-mingw32
# objdump of the cross toolchain (not necessarily in PATH)
OBJDUMP=$(command -v x86_64-w64-mingw32-objdump \
          || echo /home/z/my-project/tools/mingw/usr/bin/x86_64-w64-mingw32-objdump)
OUT=/home/z/my-project/download/releases
WORK=/tmp/usbutils-relbuild
mkdir -p "$OUT"

VERSION=$(grep -oP 'project\(USBUtils VERSION \K[0-9.]+' "$REPO/CMakeLists.txt")
echo "== USBUtils $VERSION =="

LROOT="$WORK/usbutils-$VERSION-linux-x86_64"
WROOT="$WORK/usbutils-$VERSION-windows-x86_64"
rm -rf "$LROOT" "$WROOT"
mkdir -p "$LROOT/bin" "$LROOT/lib" "$LROOT/plugins/platforms" "$LROOT/plugins/imageformats" "$LROOT/data" \
         "$WROOT/platforms" "$WROOT/styles" "$WROOT/imageformats" "$WROOT/data"

# ---------------------------------------------------------------------------
# 1) Linux package
# ---------------------------------------------------------------------------
LGUI="$REPO/build-linux/usbutils-gui"
LCLI="$REPO/build-linux/usbutils"
[ -x "$LGUI" ] && [ -x "$LCLI" ] || { echo "ERROR: Linux binaries missing (build-linux)"; exit 1; }

# Qt runtime closure (BFS over ldd), skipping glibc & the loader.
# LD_LIBRARY_PATH makes ldd resolve transitive deps that only live in the
# Qt tools dir (DT_RUNPATH of the main binary does not cover them).
export LD_LIBRARY_PATH="$QT_HOST/lib/x86_64-linux-gnu:$LROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
declare -A seen=()
queue=("$LGUI" "$QT_HOST/lib/x86_64-linux-gnu/qt6/plugins/platforms/libqxcb.so")
while [ ${#queue[@]} -gt 0 ]; do
    next=()
    for bin in "${queue[@]}"; do
        [ -f "$bin" ] || continue
        while IFS= read -r path; do
            [ -n "$path" ] || continue
            base=$(basename "$path")
            case "$base" in
                libc.so*|libm.so*|libdl*|libpthread*|librt*|ld-linux*|linux-vdso*) continue ;;
            esac
            if [ -z "${seen[$base]:-}" ]; then
                seen[$base]=1
                cp -L "$path" "$LROOT/lib/"
                next+=("$path")
            fi
        done < <(ldd "$bin" 2>/dev/null | awk '/=> \//{print $3} /^[[:space:]]*\//{print $1}')
    done
    queue=("${next[@]}")
done
[ "${#seen[@]}" -ge 10 ] || { echo "ERROR: Qt closure too small (${#seen[@]} libs)"; exit 1; }

# Bundle only the platform plugins usable by a desktop app: xcb (native
# desktop), offscreen and minimal (fallbacks / CI).  eglfs, linuxfb,
# minimalegl, vkkhrdisplay and vnc target embedded/kiosk setups and would
# drag in touchscreen/egl/vnc deps (libinput, tslib, mtdev, libproxy) that
# a desktop bundle should not have to carry.
for p in libqxcb.so libqoffscreen.so libqminimal.so; do
    cp -L "$QT_HOST/lib/x86_64-linux-gnu/qt6/plugins/platforms/$p" "$LROOT/plugins/platforms/"
done
cp -L "$QT_HOST/lib/x86_64-linux-gnu/qt6/plugins/imageformats/"*.so "$LROOT/plugins/imageformats/"

# The bundle must be self-contained: every bundled library and plugin has
# to resolve against the package itself plus the base system.  ldd resolves
# bundled deps via LD_LIBRARY_PATH (still exported from the closure walk).
missing=$(for f in "$LROOT"/lib/*.so* "$LROOT"/plugins/platforms/*.so \
                    "$LROOT"/plugins/imageformats/*.so; do
              ldd "$f" 2>/dev/null
          done | grep 'not found' | sort -u || true)
if [ -n "$missing" ]; then
    echo "ERROR: bundled Qt closure has unresolved dependencies:"
    echo "$missing"
    echo "Install the missing libs into the build host (or Qt prefix) so the"
    echo "closure can copy them, then re-run."
    exit 1
fi

cp "$LGUI" "$LROOT/bin/usbutils-gui.bin"
cp "$LCLI" "$LROOT/bin/usbutils.bin"

cat > "$LROOT/bin/usbutils" <<'EOF'
#!/usr/bin/env sh
HERE=$(cd "$(dirname "$0")/.." && pwd)
exec "$HERE/bin/usbutils.bin" "$@"
EOF
cat > "$LROOT/usbutils-gui" <<'EOF'
#!/usr/bin/env sh
HERE=$(cd "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
exec "$HERE/bin/usbutils-gui.bin" "$@"
EOF
chmod +x "$LROOT/usbutils-gui" "$LROOT/bin/usbutils"

cp "$REPO/data/PS2LOAD.ELF" "$REPO/data/usbutil.dna" "$REPO/data/usbutil.dni" \
   "$REPO/data/usbutil.ljs" "$REPO/data/usbutil.aso" "$REPO/data/usbutil.rcn" "$LROOT/data/"
mkdir -p "$LROOT/data/SCEE_DD"
cp "$REPO"/data/SCEE_DD/IOPRP*.IMG "$LROOT/data/SCEE_DD/"

# icons + desktop entry (both a layout the tarball launcher uses and the
# hicolor tree for distro-style installs)
mkdir -p "$LROOT/icons"
for s in 16 24 32 48 64 128 256; do
    cp "$REPO/res/icons/usbutils_${s}.png" "$LROOT/icons/usbutils_${s}.png"
done
cp "$REPO/res/icons/usbutils.svg" "$LROOT/icons/"
cp "$REPO/res/linux/usbutils.desktop" "$LROOT/"

cat > "$LROOT/README.txt" <<EOF
USBUtils $VERSION (Linux, x86-64)

  usbutils        command line tool      (run from any terminal)
  usbutils-gui    windowed app           (run from a file manager)
  data/           data files used by some tools:
                    PS2LOAD.ELF   payload for "ps2load inject"
                    usbutil.dni   DNAS package index (version list)
                    usbutil.dna   DNAS package data (IOPRP images)
                    usbutil.ljs   game compatibility notes
                    usbutil.aso   tool settings persistence (optional)
                    usbutil.rcn   recovery-game notes (optional)
                    SCEE_DD/      standalone IOPRP images (optional)
  icons/, usbutils.desktop
                  application icon + menu entry for distro-style installs

Quick start:
  ./usbutils drives                       # list removable drives
  ./usbutils install game.iso -d /media/usb0 -n "Game Name"
  ./usbutils list -d /media/usb0
  ./usbutils iso info game.iso            # inspect a plain ISO image
  ./usbutils support                      # check the shipped data files
  ./usbutils --help

The GUI bundles its own Qt runtime, so no Qt installation is needed.
Updates: the GUI checks automatically at startup; CLI: usbutils check-update.
EOF

tar -C "$WORK" -czf "$OUT/usbutils-$VERSION-linux-x86_64.tar.gz" "usbutils-$VERSION-linux-x86_64"
echo "Linux tarball: $(du -h "$OUT/usbutils-$VERSION-linux-x86_64.tar.gz" | cut -f1)"

# ---------------------------------------------------------------------------
# 2) Windows package
# ---------------------------------------------------------------------------
WGUI="$REPO/build-win/usbutils-gui.exe"
WCLI="$REPO/build-win/usbutils.exe"
[ -f "$WGUI" ] && [ -f "$WCLI" ] || { echo "ERROR: Windows binaries missing (build-win)"; exit 1; }
cp "$WGUI" "$WCLI" "$WROOT/"

# Copy every DLL needed at runtime, discovered by a BFS over the import
# tables of OUR executables AND of the bundled Qt/MinGW DLLs and plugins.
# A single-level walk is not enough: Qt6Core.dll itself imports
# libstdc++-6.dll, libgcc_s_seh-1.dll and libwinpthread-1.dll from the
# MinGW runtime the kit was built with. Without them, Windows loads
# whatever libstdc++-6.dll it finds on the machine (e.g. an MSYS2 one with
# a different GCC) — or the app fails to start at all on clean machines.
declare -A dllsrc=()
for d in "$QT_WIN/bin" "$MINGW/lib"; do
    while IFS= read -r f; do
        dllsrc["$(basename "$f" | tr 'A-Z' 'a-z')"]="$(readlink -f "$f")"
    done < <(find "$d" -maxdepth 1 -name '*.dll' 2>/dev/null)
done

imports_of() {  # imports_of <pe-file> — list imported DLL names
    "$OBJDUMP" -p "$1" 2>/dev/null | awk '/DLL Name/{print $3}'
}

# copy the plugins FIRST so their imports join the walk
cp -L "$QT_WIN/plugins/platforms/qwindows.dll"      "$WROOT/platforms/"
cp -L "$QT_WIN/plugins/styles/qmodernwindowsstyle.dll" "$WROOT/styles/" 2>/dev/null || true
for q in qgif qico qjpeg; do
    cp -L "$QT_WIN/plugins/imageformats/$q.dll" "$WROOT/imageformats/" 2>/dev/null || true
done

copied=0
declare -A queued=()
queue=("$WGUI" "$WCLI" "$WROOT/platforms/qwindows.dll")
[ -f "$WROOT/styles/qmodernwindowsstyle.dll" ] && queue+=("$WROOT/styles/qmodernwindowsstyle.dll")
while [ ${#queue[@]} -gt 0 ]; do
    next=()
    for bin in "${queue[@]}"; do
        while IFS= read -r dll; do
            [ -n "$dll" ] || continue
            case "$dll" in
                KERNEL32.dll|USER32.dll|GDI32.dll|SHELL32.dll|ADVAPI32.dll|WS2_32.dll|\
                WINHTTP.dll|ole32.dll|OLEAUT32.dll|COMCTL32.dll|UXTHEME.dll|CRYPT32.dll|\
                Secur32.dll|bcrypt.dll|ntdll.dll|msvcrt.dll|SHLWAPI.dll|IMM32.dll|\
                WINMM.dll|dwmapi.dll|dbghelp.dll|USERENV.dll|NETAPI32.dll|d3d11.dll|\
                dxgi.dll|glu32.dll|OPENGL32.dll|api-ms-*|ext-ms-*) continue ;;
            esac
            key="${dll,,}"
            [ -n "${queued[$key]:-}" ] && continue
            queued[$key]=1
            src="${dllsrc[$key]:-}"
            if [ -n "$src" ]; then
                cp -L "$src" "$WROOT/" && copied=$((copied+1))
                next+=("$src")
            else
                echo "  (system DLL, not bundled: $dll)"
            fi
        done < <(imports_of "$bin")
    done
    queue=("${next[@]}")
done
echo "Windows: bundled $copied DLLs (incl. MinGW runtime)"
cp "$REPO/data/PS2LOAD.ELF" "$REPO/data/usbutil.dna" "$REPO/data/usbutil.dni" \
   "$REPO/data/usbutil.ljs" "$REPO/data/usbutil.aso" "$REPO/data/usbutil.rcn" "$WROOT/data/"
mkdir -p "$WROOT/data/SCEE_DD"
cp "$REPO"/data/SCEE_DD/IOPRP*.IMG "$WROOT/data/SCEE_DD/"

cat > "$WROOT/README.txt" <<EOF
USBUtils $VERSION (Windows, x86-64)

  usbutils.exe        command line tool   (run from cmd / PowerShell)
  usbutils-gui.exe    windowed app        (double-click it)
  data/               data files used by some tools:
                        PS2LOAD.ELF   payload for "ps2load inject"
                        usbutil.dni   DNAS package index (version list)
                        usbutil.dna   DNAS package data (IOPRP images)
                        usbutil.ljs   game compatibility notes
                        usbutil.aso   tool settings persistence (optional)
                        usbutil.rcn   recovery-game notes (optional)
                        SCEE_DD/      standalone IOPRP images (optional)

Quick start (cmd):
  usbutils.exe drives
  usbutils.exe install game.iso -d E:\ -n "Game Name"
  usbutils.exe list -d E:\
  usbutils.exe iso info game.iso
  usbutils.exe support

Install straight from a disc: usbutils.exe install D: -d E:\ -n "Game"

All required Qt and MinGW runtime DLLs are included in this folder.
Keep the exe files together with the DLL, platforms, styles and
imageformats folders. Updates: the GUI checks automatically at startup;
CLI: usbutils.exe check-update.
EOF

rm -f "$OUT/usbutils-$VERSION-windows-x86_64.zip"
(cd "$WORK" && zip -qr "$OUT/usbutils-$VERSION-windows-x86_64.zip" "usbutils-$VERSION-windows-x86_64")
echo "Windows zip: $(du -h "$OUT/usbutils-$VERSION-windows-x86_64.zip" | cut -f1)"

# ---------------------------------------------------------------------------
# 3) sanity checks
# ---------------------------------------------------------------------------
echo "== sanity =="
[ "$(tar tzf "$OUT/usbutils-$VERSION-linux-x86_64.tar.gz" | grep -c 'lib/libQt6Core.so')" -ge 1 ] || { echo "FAIL: no Qt in linux tarball"; exit 1; }
[ "$(tar tzf "$OUT/usbutils-$VERSION-linux-x86_64.tar.gz" | grep -c 'data/usbutil.dna')" -ge 1 ] || { echo "FAIL: no usbutil.dna in linux tarball"; exit 1; }
[ "$(tar tzf "$OUT/usbutils-$VERSION-linux-x86_64.tar.gz" | grep -c 'data/usbutil.rcn')" -ge 1 ] || { echo "FAIL: no usbutil.rcn in linux tarball"; exit 1; }
[ "$(tar tzf "$OUT/usbutils-$VERSION-linux-x86_64.tar.gz" | grep -c 'SCEE_DD/IOPRP165.IMG')" -ge 1 ] || { echo "FAIL: no IOPRP images in linux tarball"; exit 1; }
[ "$(tar tzf "$OUT/usbutils-$VERSION-linux-x86_64.tar.gz" | grep -c 'usbutils.desktop')" -ge 1 ] || { echo "FAIL: no .desktop in linux tarball"; exit 1; }
[ "$(unzip -l "$OUT/usbutils-$VERSION-windows-x86_64.zip" | grep -c 'usbutil.dna')" -ge 1 ] || { echo "FAIL: no usbutil.dna in windows zip"; exit 1; }
[ "$(unzip -l "$OUT/usbutils-$VERSION-windows-x86_64.zip" | grep -c 'usbutil.rcn')" -ge 1 ] || { echo "FAIL: no usbutil.rcn in windows zip"; exit 1; }
[ "$(unzip -l "$OUT/usbutils-$VERSION-windows-x86_64.zip" | grep -c 'IOPRP202.IMG')" -ge 1 ] || { echo "FAIL: no IOPRP images in windows zip"; exit 1; }
[ "$(unzip -l "$OUT/usbutils-$VERSION-windows-x86_64.zip" | grep -c 'Qt6Core.dll')" -ge 1 ] || { echo "FAIL: no Qt6Core.dll in windows zip"; exit 1; }
[ "$(unzip -l "$OUT/usbutils-$VERSION-windows-x86_64.zip" | grep -c 'libstdc++-6.dll')" -ge 1 ] || { echo "FAIL: no libstdc++-6.dll in windows zip"; exit 1; }
[ "$(unzip -l "$OUT/usbutils-$VERSION-windows-x86_64.zip" | grep -c 'libgcc_s_seh-1.dll')" -ge 1 ] || { echo "FAIL: no libgcc_s_seh-1.dll in windows zip"; exit 1; }
[ "$(unzip -l "$OUT/usbutils-$VERSION-windows-x86_64.zip" | grep -c 'libwinpthread-1.dll')" -ge 1 ] || { echo "FAIL: no libwinpthread-1.dll in windows zip"; exit 1; }
echo "ALL PACKAGE CHECKS PASSED"
