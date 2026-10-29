#!/usr/bin/env sh
# Launch script used when USBUtils is installed from the release tarball
# layout (bin/ + lib/ + plugins/ next to each other). Finds its own root,
# exports the bundled Qt runtime, then execs the real GUI binary.
HERE=$(cd "$(dirname "$0")/.." && pwd)
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
exec "$HERE/bin/usbutils-gui.bin" "$@"
