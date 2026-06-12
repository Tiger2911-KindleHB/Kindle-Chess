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
- Board flip
- Confirmed New Game flow
- Resign button
- Exit button
- Optional board coordinates
- Optional move-history panel
- Settings overlay
- UI font-size slider for larger Kindle-friendly text
- Save/resume using `/mnt/us/extensions/kindlechess/data/save.txt`
- Persistent settings using `/mnt/us/extensions/kindlechess/data/settings.txt`
- Optional local UCI engine support
- Designed for slow, high-contrast E Ink interaction

## Basic controls

- **New**: asks for confirmation, then resets the board.
- **Undo**: takes back the previous move.
- **Flip**: flips board orientation.
- **Resign**: ends the current local game by resignation.
- **Settings**: opens toggles and the UI font-size slider.
- **Engine**: cycles Off / Black / White. This only works after a UCI engine is added.
- **Level**: cycles engine move time.
- **Exit**: saves and returns to KUAL.

In **Settings**, tap the font-size slider line to resize the main UI text. Settings are saved automatically.

## Engine support

Stockfish is not bundled. Put a Kindle-compatible UCI engine binary at:

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

Difficulty is controlled by move time:

```text
250 ms / 750 ms / 1500 ms / 3000 ms
```


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

The workflow downloads the prebuilt `kindlehf` koxtoolchain release, installs the Kindle SDK into it, builds with Meson, packages the KUAL extension, and uploads `kindlechess-kual.zip` as a workflow artifact.

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

At the end, note the path printed for `meson-crosscompile.txt`. It is commonly similar to:

```text
~/x-tools/arm-kindlehf-linux-gnueabihf/meson-crosscompile.txt
```

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

## Optional: build Stockfish for Kindle

Stockfish cross-compilation is the least certain part because exact toolchain variables can differ. The app does not require Stockfish to launch.

General shape:

```bash
git clone https://github.com/official-stockfish/Stockfish.git
cd Stockfish/src
make clean
make -j$(nproc) build ARCH=armv7 COMP=gcc \
  CXX=~/x-tools/arm-kindlehf-linux-gnueabihf/bin/arm-kindlepw2-linux-gnueabihf-g++
```

If the compiler path differs, inspect:

```bash
ls ~/x-tools/arm-kindlehf-linux-gnueabihf/bin
```

Then copy the produced binary to:

```text
/mnt/us/extensions/kindlechess/bin/stockfish
```

and make it executable:

```bash
chmod +x /mnt/us/extensions/kindlechess/bin/stockfish
```

If the newest Stockfish does not run on Kindle, try an older Stockfish release or a smaller UCI engine such as Fairy-Stockfish/older GNU Chess built for ARM hard-float.

## Local desktop test

On Linux with GTK2 dev packages installed:

```bash
meson setup builddir
meson compile -C builddir
./builddir/kindlechess
```

This is useful for testing rules/UI before cross-compiling.


## Exiting

Tap the **Exit** button in the top toolbar. The game autosaves before quitting.


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
high-contrast black/white artwork
```

Required short filenames:

```text
wk.png  wq.png  wr.png  wb.png  wn.png  wp.png
bk.png  bq.png  br.png  bb.png  bn.png  bp.png
```

Long filenames are also accepted:

```text
white_king.png   white_queen.png   white_rook.png
white_bishop.png white_knight.png  white_pawn.png
black_king.png   black_queen.png   black_rook.png
black_bishop.png black_knight.png  black_pawn.png
```

The Settings menu includes:

```text
Piece PNGs: On/Off
Reload PNGs
```

If a PNG is missing or invalid, the app falls back to built-in letter pieces for that specific piece.
