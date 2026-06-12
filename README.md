# KindleChess

A native chess app for jailbroken Kindle devices, designed for KUAL launch, E Ink displays, touch-first play, custom chess-piece PNGs, and optional local Stockfish engine support.

KindleChess is intentionally built like a Kindle app, not like a tablet game: high contrast, no animation loop, large touch targets, persistent saves, and minimal redraws.

---

## Highlights

- Native C++17 / GTK2 Kindle application
- KUAL-launchable homebrew extension
- Full legal chess move handling
- Optional local Stockfish/UCI engine support
- Approximate Elo-based engine difficulty
- Custom PNG chess-piece themes
- Captured-piece display
- Move history and review mode
- PGN export
- Five manual save slots
- Autosave and resume
- Large-font Kindle-friendly UI
- Optional move confirmation
- Dark-mode piece visibility correction

---

## Current feature set

### Chess rules and board UI

- Legal move generation
- Check, checkmate, and stalemate handling
- Castling
- En passant
- Promotion selector
- Legal-move highlighting
- Last-move highlighting
- Board coordinates toggle
- Move-history panel toggle
- Captured-piece display
- Game-over popup with New Game action

### Engine support

KindleChess can launch a local Stockfish binary as a child process and communicate with it through UCI.

Supported engine features:

- Engine Off / Black / White
- Approximate Elo selection in 250-point steps
- Engine side and Elo lock after the first move
- Hint button
- Restart Engine maintenance action
- Conservative Kindle-safe engine defaults

Default engine settings:

```text
Threads = 1
Hash = 16 MB
Ponder = false
MultiPV = 1
```

Stockfish is expected at:

```text
/mnt/us/extensions/kindlechess/bin/stockfish
```

### Save, export, and review

- Autosave current game
- Five manual save slots
- Save / Load / Delete slot controls
- PGN export
- Review mode with previous/next move navigation

Data is stored under:

```text
/mnt/us/extensions/kindlechess/data/
```

---

## Controls

The top toolbar is intentionally minimal:

```text
New | Undo | Settings | Exit
```

### New

Starts a new game after confirmation.

### Undo

Takes back the previous move. When playing against the engine, KindleChess attempts to undo the engine reply as well.

### Settings

Opens all display, engine, save/load, PGN, hint, review, and custom-piece options.

### Exit

Autosaves and quits back to KUAL. If Stockfish is running, KindleChess sends `quit` and follows up with process cleanup if needed.

---

## Settings menu

Available settings/actions include:

- **Engine** — Off / Black / White
- **Elo -250 / Elo +250** — adjust approximate engine strength
- **Hint** — asks Stockfish for a suggested move and highlights it
- **Restart Engine** — stops the Stockfish child process so it can relaunch cleanly
- **Export PGN** — writes the current game as a PGN file
- **Confirm Moves** — enables/disables move confirmation popups
- **Save / Load** — opens the five-slot save manager
- **Review Game** — enters move-review mode
- **Coordinates** — toggles board coordinate labels
- **Move List** — toggles the move-history panel
- **A- / A+** — adjusts UI font size
- **Piece PNGs** — toggles custom PNG chess pieces
- **Reload PNGs** — reloads piece images from storage
- **Dark Piece Fix** — Auto / Off / On piece inversion for Kindle dark mode

Engine side and Elo can only be changed before the first move. Start a new game to unlock those options.

---

## Installation

Build or download the KUAL package, then copy the extracted `kindlechess` folder to:

```text
/mnt/us/extensions/kindlechess
```

Final Kindle layout:

```text
/mnt/us/extensions/kindlechess/
  config.xml
  menu.json
  bin/
    start.sh
    kindlechess
    stockfish          # optional, if engine support is bundled
  data/
  pieces/
    custom/
```

Launch from:

```text
KUAL → KindleChess
```

---

## Custom chess-piece PNGs

KindleChess supports user-supplied chess-piece images.

Put custom pieces here:

```text
/mnt/us/extensions/kindlechess/pieces/custom/
```

Recommended image format:

```text
256 x 256 px
PNG
transparent background
square canvas
centered piece art
high contrast for E Ink
```

Required short filenames:

```text
wk.png  wq.png  wr.png  wb.png  wn.png  wp.png
bk.png  bq.png  br.png  bb.png  bn.png  bp.png
```

Long filenames are also supported:

```text
white_king.png    white_queen.png   white_rook.png
white_bishop.png  white_knight.png  white_pawn.png
black_king.png    black_queen.png   black_rook.png
black_bishop.png  black_knight.png  black_pawn.png
```

After copying images, open KindleChess and use:

```text
Settings → Reload PNGs
```

If pieces are hard to see in Kindle dark mode, use:

```text
Settings → Dark Piece Fix → On
```

---

## File locations

```text
/mnt/us/extensions/kindlechess/data/save.txt         Autosave
/mnt/us/extensions/kindlechess/data/settings.txt     Persistent settings
/mnt/us/extensions/kindlechess/data/engine.log       Engine diagnostics
/mnt/us/extensions/kindlechess/data/games/           PGN exports
/mnt/us/extensions/kindlechess/data/slots/           Manual save slots
/mnt/us/extensions/kindlechess/pieces/custom/        Custom piece PNGs
```

---

## GitHub Actions build

This project can be built in GitHub Actions using an Ubuntu runner, the Kindle `kindlehf` toolchain, and the Kindle SDK.

Recommended repository layout:

```text
.github/
  workflows/
    main.yml
src/
extension/
scripts/
meson.build
meson.options
README.md
```

Build steps:

1. Push the project to GitHub.
2. Open the repository's **Actions** tab.
3. Run **Build KindleChess for Kindle**.
4. Download the `kindlechess-kual` artifact.
5. Extract the artifact.
6. Copy the resulting `kindlechess` folder to the Kindle's `extensions` folder.

For public repositories, standard GitHub-hosted Actions runners are generally free to use.

---

## Local build notes

The easiest local build environment on Windows is WSL2 Ubuntu. Native Windows/PowerShell builds are not recommended because the target is an ARM Linux Kindle binary.

### Target selection

```text
kindlehf   Firmware 5.16.3+
kindlepw2  Older PW2-or-newer firmware below 5.16.3
```

For a Kindle Paperwhite 12th generation, use:

```text
kindlehf
```

### Required host packages

```bash
sudo apt update
sudo apt install -y \
  build-essential autoconf automake bison flex gawk \
  libtool libtool-bin libncurses-dev curl file git gperf \
  help2man texinfo unzip wget sed libarchive-dev nettle-dev \
  meson ninja-build pkg-config zip python3 make binutils \
  gtk2.0 libgtk2.0-dev
```

### Build koxtoolchain

```bash
git clone --recursive --depth=1 https://github.com/koreader/koxtoolchain.git
cd koxtoolchain
chmod +x ./gen-tc.sh
./gen-tc.sh kindlehf
```

### Install Kindle SDK

```bash
cd ~
git clone --recursive --depth=1 https://github.com/KindleModding/kindle-sdk.git
cd kindle-sdk
chmod +x ./gen-sdk.sh
./gen-sdk.sh kindlehf
```

### Build KindleChess

From the project root:

```bash
meson setup --cross-file ~/x-tools/arm-kindlehf-linux-gnueabihf/meson-crosscompile.txt builddir_kindlehf
meson compile -C builddir_kindlehf
```

### Package as a KUAL extension

```bash
./scripts/package-kual.sh ./builddir_kindlehf/kindlechess
```

The package is written to:

```text
dist/kindlechess-kual.zip
```

---

## Troubleshooting

### Engine says it did not answer `uciok`

Check:

```text
/mnt/us/extensions/kindlechess/data/engine.log
```

Common causes:

- `stockfish` is missing
- `stockfish` is not executable
- `stockfish` was built for the wrong CPU/architecture
- dynamic runtime dependency mismatch
- Stockfish crashed before answering UCI startup

### Custom pieces do not show

Check that files are in:

```text
/mnt/us/extensions/kindlechess/pieces/custom/
```

Then use:

```text
Settings → Piece PNGs → On
Settings → Reload PNGs
```

If one piece is missing or invalid, KindleChess falls back to the built-in piece rendering for that piece.

### Font or layout looks wrong

Settings are persisted here:

```text
/mnt/us/extensions/kindlechess/data/settings.txt
```

Delete that file to reset display settings to defaults.

---

## Design goals

KindleChess is optimized for:

- E Ink readability
- low redraw frequency
- large touch targets
- local/offline play
- stable KUAL launch behavior
- battery-conscious engine usage
- user-customizable piece themes

It intentionally avoids:

- online chess services
- browser/webview runtime
- animations
- continuous engine analysis by default
- crowded top-bar controls

---

## License notes

A project license file should be added before public release.

If distributing a build that includes Stockfish, comply with Stockfish's GPLv3 license requirements and include the relevant license and attribution files in the packaged extension.

---

## Status

KindleChess is under active homebrew development and is currently targeted at jailbroken Kindle devices launched through KUAL.
