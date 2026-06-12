#!/bin/sh
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$ROOT/builddir_kindlehf/kindlechess}"
OUT="$ROOT/dist"
EXT="$OUT/kindlechess"
rm -rf "$OUT"
mkdir -p "$EXT/bin" "$EXT/data"
cp "$ROOT/extension/kindlechess/config.xml" "$EXT/config.xml"
cp "$ROOT/extension/kindlechess/menu.json" "$EXT/menu.json"
cp "$ROOT/extension/kindlechess/bin/start.sh" "$EXT/bin/start.sh"
cp "$BIN" "$EXT/bin/kindlechess"
chmod +x "$EXT/bin/start.sh" "$EXT/bin/kindlechess"
cat > "$EXT/README-INSTALL.txt" <<'TXT'
Copy this entire kindlechess folder to:

  /mnt/us/extensions/kindlechess

Then open KUAL and launch KindleChess.

Optional engine:
  Put a Kindle-compatible Stockfish/UCI binary here:
  /mnt/us/extensions/kindlechess/bin/stockfish
  chmod +x /mnt/us/extensions/kindlechess/bin/stockfish
TXT
( cd "$OUT" && zip -r kindlechess-kual.zip kindlechess >/dev/null )
echo "Created: $OUT/kindlechess-kual.zip"
