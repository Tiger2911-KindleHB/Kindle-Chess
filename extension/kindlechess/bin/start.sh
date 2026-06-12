#!/bin/sh
HERE="$(dirname "$0")"
EXTDIR="$(cd "$HERE/.." && pwd)"
cd "$EXTDIR" || exit 1

export KINDLECHESS_HOME="$EXTDIR"
export KINDLECHESS_ENGINE="$EXTDIR/bin/stockfish"
export LD_LIBRARY_PATH="$EXTDIR/lib:$LD_LIBRARY_PATH"

# If a bundled engine is present but lacks executable bit, fix it.
[ -f "$KINDLECHESS_ENGINE" ] && chmod +x "$KINDLECHESS_ENGINE"
[ -f "$EXTDIR/bin/kindlechess" ] && chmod +x "$EXTDIR/bin/kindlechess"

"$EXTDIR/bin/kindlechess" >> "$EXTDIR/data/kindlechess.log" 2>&1
