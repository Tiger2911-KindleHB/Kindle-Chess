# KindleChess

A native chess application for jailbroken Kindle devices.

KindleChess is designed specifically for E Ink displays, providing a clean, distraction-free chess experience with touch controls, local engine support, custom piece themes, and persistent game management. Unlike web-based ports, KindleChess runs natively on the Kindle and is optimized for readability, battery life, and long-form play.

---

## Features

### Chess Gameplay

* Full chess rules and move validation
* Legal move highlighting
* Move history tracking
* Undo support
* Check and checkmate detection
* Pawn promotion support
* Review completed games

### Stockfish Engine

* Integrated local Stockfish engine
* Adjustable engine strength (Elo-based)
* Play as White or Black
* In-game hint system
* Engine restart option
* Fully offline gameplay

### Kindle-Friendly Interface

* Large touch targets
* Adjustable font sizes
* High-contrast E Ink design
* Minimal screen refreshes
* Optimized for battery life
* Dark mode compatibility options

### Custom Piece Themes

* Support for custom PNG chess pieces
* User-replaceable piece artwork
* Automatic fallback to built-in pieces
* Promotion menu supports custom piece graphics

### Save & Export

* Automatic game saving
* Multiple manual save slots
* PGN export support
* Review mode for completed games

---

## Installation

1. Install KUAL on your jailbroken Kindle.
2. Download the latest KindleChess release.
3. Copy the `kindlechess` folder into:

```text
/mnt/us/extensions/
```

4. Launch KindleChess from KUAL.

---

## Custom Chess Pieces

KindleChess supports custom PNG piece sets.

Place your piece files in:

```text
/mnt/us/extensions/kindlechess/pieces/custom/
```

Supported filenames:

```text
wk.png  wq.png  wr.png  wb.png  wn.png  wp.png
bk.png  bq.png  br.png  bb.png  bn.png  bp.png
```

Recommended format:

* PNG
* Transparent background
* 256×256 pixels
* Centered artwork

---

## Exported Files

Game data is stored inside:

```text
/mnt/us/extensions/kindlechess/data/
```

This includes:

* Autosaves
* Manual save slots
* PGN exports
* Engine logs
* User settings

---

## Project Goals

KindleChess aims to provide the best native chess experience available for jailbroken Kindle devices by focusing on:

* Simplicity
* Performance
* Readability
* Offline functionality
* Long battery life
* Community customization

---

## Disclaimer

KindleChess is an unofficial community project and is not affiliated with Amazon, Kindle, Stockfish, Chess.com, or any other chess platform.

Use at your own risk on jailbroken devices.

---

## License

See the repository license file for licensing information.

Stockfish is distributed under its own license and remains the property of its respective contributors.
