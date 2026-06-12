#!/bin/sh
set -eu
CROSS_FILE="${1:-$HOME/x-tools/arm-kindlehf-linux-gnueabihf/meson-crosscompile.txt}"
rm -rf builddir_kindlehf
meson setup --cross-file "$CROSS_FILE" builddir_kindlehf
meson compile -C builddir_kindlehf
