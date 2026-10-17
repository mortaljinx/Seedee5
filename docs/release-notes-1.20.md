## Seedee5 1.20

A Philips CD-i emulator for the PS5, as a native homebrew title (PPSA99142).

### New: game cards that tell you about the game

- **About the game:** opening a game shows a few lines from its Wikipedia article, credited to the article (Wikipedia's text is CC BY-SA 4.0).
- **Review scores:** where the article lists them (AllGame, Edge, CD-i Magazine, Next Generation and others).
- **Backdrops and logos:** the card sits over a dimmed in-game screenshot, and the game's own logo replaces its name. Both come from libretro-thumbnails and are checked against that collection's file list before use.
- **Works offline after the first look:** everything is cached on the console, so a card opens at once the second time. Settings → "Download covers and game info" turns all downloads off.

### Also

- The project is now **Seedee5**, with its new logo as the icon and on the wheel.
- A sound diagnostic in the log (`cdi_log.txt`): what the CD-i's sound chip played and whether it ran dry.

### Install

1. Download `Seedee5-1.20.zip` below and unzip it. It holds one folder, `PPSA99142`.
2. Copy `PPSA99142` over FTP to `/data/homebrew/` on your PS5. **Updating:** copy it over the old folder; don't delete the old one first, because your saves and settings live in its `data` folder.
3. On a new install, put the CD-i BIOS (`cdimono1.zip`, not included) in `PPSA99142/data/system/same_cdi/bios/`, and your games in `PPSA99142/data/roms/`, on a USB drive, or on a NAS.

Full instructions are in `INSTALL.txt` inside the zip and in the README.

No BIOS, games or keys are included. Check the downloads against `SHA256SUMS`.

See [CHANGELOG.md](CHANGELOG.md) for earlier versions.
