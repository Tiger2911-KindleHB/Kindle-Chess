# KindleChess

Native GTK2 chess app for jailbroken Kindle devices, intended for KUAL launch.

## Features

- Native C++17 / GTK2 UI
- Full legal move generation: check, checkmate, stalemate, castling, en passant, promotion
- Tap source square, tap destination square
- Legal destination highlighting
- Last-move highlighting
- Promotion selector
- Undo
- Confirmed New Game flow
- Exit button
- Optional board coordinates
- Optional move-history panel
- Settings overlay
- A-/A+ UI font controls, up to size 50
- Save/resume using `/mnt/us/extensions/kindlechess/data/save.txt`
- Persistent settings using `/mnt/us/extensions/kindlechess/data/settings.txt`
- Optional local Stockfish/UCI engine support
- Engine side and approximate Elo configured in Settings before the first move
- Checkmate/stalemate game-over popup with New Game button
- Optional custom PNG chess pieces
- Designed for slow, high-contrast E Ink interaction

## Basic controls

- **New**: asks for confirmation, then resets the board.
- **Undo**: takes back the previous move. When playing against the engine, it attempts to take back the engine reply too.
- **Settings**: opens display, engine, Elo, and piece-image options.
- **Exit**: saves and returns to KUAL.

Removed from the top bar in this build:

- Flip
- Resign
- Level/millisecond control

## Settings

Settings include:

- **Engine**: cycles Off / Black / White.
- **Elo -250 / Elo +250**: changes the engine strength display in 250 Elo steps.
- **Coordinates**: toggles board coordinate labels.
- **Move List**: toggles the move-history panel.
- **A- / A+**: changes UI font size from 14 up to 50.
- **Piece PNGs**: toggles custom piece images.
- **Reload PNGs**: reloads custom piece images from storage.

Engine side and Elo can only be changed before the first move of the game. After the first move, the Settings screen shows them as locked. Start a new game to change them.

## Engine support

Stockfish is expected at:

```text
/mnt/us/extensions/kindlechess/bin/stockfish
```

The app launches it locally and sends UCI commands. Conservative settings are hardcoded:

```text
Threads = 1
Hash = 16 MB
Ponder = false
MultiPV = 1
```

Engine strength is shown to the user as approximate Elo:

```text
250 / 500 / 750 / 1000 / 1250 / 1500 / 1750 / 2000 / 2250 / 2500 / 2750 / 3000
```

Internally, KindleChess uses that Elo setting to configure Stockfish with `Skill Level`, `UCI_LimitStrength`, and `UCI_Elo` where supported, and uses a bounded move-time search so the Kindle remains responsive.

## GitHub Actions cloud build

This repo includes `.github/workflows/build-kindlehf.yml`.

Use a public GitHub repository for free standard GitHub-hosted runner usage.

Steps:

1. Create a GitHub repository.
2. Upload this project so `meson.build`, `src/`, `extension/`, `scripts/`, and `.github/` are at the repository root.
3. Open the repository on GitHub.
4. Go to **Actions**.
5. Select **Build KindleChess for Kindle**.
6. Click **Run workflow**.
7. When it finishes, open the workflow run.
8. Download the `kindlechess-kual` artifact.
9. Unzip it and copy the `kindlechess` folder to `/mnt/us/extensions/kindlechess` on the Kindle.

## Build for Kindle

Target selection:

- Firmware >= 5.16.3: `kindlehf`
- Firmware < 5.16.3 on PW2 or newer: `kindlepw2`

The Paperwhite 12th gen will normally be `kindlehf` unless you are running unusual old firmware.

### Ubuntu/WSL packages

```bash
sudo apt update
sudo apt install -y build-essential autoconf automake bison flex gawk libtool libtool-bin libncurses-dev curl file git gperf help2man texinfo unzip wget sed libarchive-dev nettle-dev meson gtk2.0 libgtk2.0-dev zip
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

At the end, note the path printed for `meson-crosscompile.txt`.

### Build KindleChess

From this project directory:

```bash
meson setup --cross-file ~/x-tools/arm-kindlehf-linux-gnueabihf/meson-crosscompile.txt builddir_kindlehf
meson compile -C builddir_kindlehf
```

### Package as a KUAL extension

```bash
./scripts/package-kual.sh ./builddir_kindlehf/kindlechess
```

This creates:

```text
dist/kindlechess-kual.zip
```

Unzip it and copy the `kindlechess` folder to the Kindle:

```text
/mnt/us/extensions/kindlechess
```

Then launch it from KUAL.

## Exiting

Tap the **Exit** button in the top toolbar. The game autosaves before quitting. The engine child process is sent `quit`; if needed, the app follows up with SIGTERM/SIGKILL.

## Custom chess-piece PNGs

KindleChess can use user-supplied PNG images for the pieces. Put transparent PNGs here on the Kindle:

```text
/mnt/us/extensions/kindlechess/pieces/custom/
```

Recommended dimensions:

```text
256 x 256 px
PNG
transparent background
square canvas
centered piece art
```

Required short filenames:

```text
wk.png  wq.png  wr.png  wb.png  wn.png  wp.png
bk.png  bq.png  br.png  bb.png  bn.png  bp.png
```

Long filenames also work:

```text
white_king.png    white_queen.png   white_rook.png
white_bishop.png  white_knight.png  white_pawn.png
black_king.png    black_queen.png   black_rook.png
black_bishop.png  black_knight.png  black_pawn.png
```
