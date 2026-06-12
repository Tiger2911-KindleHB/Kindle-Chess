#!/bin/sh
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$ROOT/builddir_kindlehf/kindlechess}"
OUT="$ROOT/dist"
EXT="$OUT/kindlechess"
rm -rf "$OUT"
mkdir -p "$EXT/bin" "$EXT/data" "$EXT/pieces/custom" "$EXT/pieces/default"
cp "$ROOT/extension/kindlechess/config.xml" "$EXT/config.xml"
cp "$ROOT/extension/kindlechess/menu.json" "$EXT/menu.json"
cp "$ROOT/extension/kindlechess/bin/start.sh" "$EXT/bin/start.sh"
cp "$BIN" "$EXT/bin/kindlechess"
if [ -d "$ROOT/extension/kindlechess/pieces" ]; then cp -R "$ROOT/extension/kindlechess/pieces/." "$EXT/pieces/"; fi
chmod +x "$EXT/bin/start.sh" "$EXT/bin/kindlechess"
cat > "$EXT/README-INSTALL.txt" <<'TXT'
Copy this entire kindlechess folder to:

  /mnt/us/extensions/kindlechess

Then open KUAL and launch KindleChess.

Custom piece PNGs:
  Put transparent PNG chess-piece images here:
  /mnt/us/extensions/kindlechess/pieces/custom
  Required short names:
    wk.png wq.png wr.png wb.png wn.png wp.png
    bk.png bq.png br.png bb.png bn.png bp.png
  Recommended size: 256x256 PNG, transparent background.

Game data:
  Autosave: /mnt/us/extensions/kindlechess/data/save.txt
  Save slots: /mnt/us/extensions/kindlechess/data/slots
  PGN exports: /mnt/us/extensions/kindlechess/data/games

Optional engine:
  Put a Kindle-compatible Stockfish/UCI binary here:
  /mnt/us/extensions/kindlechess/bin/stockfish
  chmod +x /mnt/us/extensions/kindlechess/bin/stockfish
TXT
( cd "$OUT" && zip -r kindlechess-kual.zip kindlechess >/dev/null )
echo "Created: $OUT/kindlechess-kual.zip"
