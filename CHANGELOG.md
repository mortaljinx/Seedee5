# Changelog

## 1.22 (2026-10-20)

- **Updating no longer loses your settings and saved games.** ProsperoStore updates an app by replacing its
  whole folder (the old one, with your files, goes to `/data/prosperostore/previous/PPSA99142`), and everything
  Seedee5 keeps was inside it. Seedee5 now also keeps your settings, NAS settings, saved games and BIOS in the
  title's own storage (`/download0`), which an update doesn't touch, and puts them back by itself when it finds
  its folder replaced. Games on the console, covers and save states are too big for that storage: the wheel then
  says where they are, to copy back over FTP. (Updating from 1.21 or older, the wheel says where the old data
  folder went, as nothing was kept yet.)
- **Finding a NAS on another subnet.** *Find NAS on my network* only tried the console's own /24 (10.0.3.x for a
  console at 10.0.3.21), so a NAS at 10.0.0.8 on the same network with a wider mask was never found. It now also
  asks the whole network by broadcast. A NAS behind a router on a separate network still can't be found that way:
  *Enter the address* takes it, and now starts from the console's own network instead of 192.168.1.1.
- Cover and game-info downloads ask the router of the console's own network for names (x.y.z.1), not
  192.168.1.1, before 1.1.1.1 and 8.8.8.8.
- A message about NAS save backups could crash the app when the NAS held no saves yet.

## 1.21 (2026-10-17)

- **New logo:** the S5 icon on the PS5 home screen, at the top left of the wheel and in the README.
- **Safer downloads:** a cover or game-info download that is redirected now only follows the redirect within the
  same site or to Wikipedia, GitHub or libretro, never to another address (such as a device on your network).
- The release's source archive holds exactly the files in the repository.
- Docs: the title folder is shown as `~/ps5/Seedee5`.

## 1.20 (2026-10-17)

- **Game cards tell you about the game.** Opening a game shows a few lines about it from its Wikipedia article,
  its review scores where the article lists them (AllGame, Edge, CD-i Magazine and others), and a credit naming
  the article (Wikipedia's text is CC BY-SA 4.0). The list's own title is tried first, then the file's, then a
  search; only an article about a video game is used.
- **Backdrops and logos.** The card sits over a dimmed in-game screenshot of the game, and the game's logo
  replaces its name as the title, both from libretro-thumbnails and checked against the collection's file list.
- Everything is cached on the console (data/covers/extras), so a card opens at once the second time and works
  offline. "Download missing covers" in Settings is now "Download covers and game info" and covers both.
- The project is now called Seedee5, with its new logo as the icon and on the wheel.

## 1.11 (2026-10-13)

- **FMV clean-up keeps text sharp.** The video is still smoothed, but text and the CD-i's drawn graphics (the start
  screen, menus, scores) are now upscaled as crisp pixels: they are told from video by their edges, which go from
  one colour to the other in a single pixel (MPEG spreads video's edges over two or more). 1.10 blurred them.

## 1.10 (2026-10-13)

- **Picture filters at 4K.** Every filter now draws at the screen's own 3840x2160 instead of 1920x1080 stretched
  by two; the scanlines, the CRT's mask and Pixel art's edges are drawn at the TV's real pixels. (If the console
  cannot make images that large, the filters fall back to 1080p, as before.)
- **A proper CRT.** Beams that widen with brightness, PAL-like colour bleed along the lines, an aperture-grille
  phosphor mask at one stripe per screen pixel, glow around bright parts, a gently curved screen with rounded
  corners, and a vignette.
- **FMV clean-up (light, medium, strong).** A new Picture setting for the Digital Video games: MPEG's 8x8 blocks,
  16x16 colour blocks and ringing are removed at the video's own size (non-local means, brightness and colour
  treated separately), then the picture is upscaled without halos. In tests on a frame compressed as a CD-i's
  MPEG is, it beat a small AI super-resolution network, which kept the blocks.
- The log says what the display is and whether the console offers what HDR would need.
- Settings shows the version; the Recd-i logo sits at the top left of the wheel.

## 1.00 (2026-10-12)

First release.

- Philips CD-i emulation on the PS5 with the SAME_CDI core, as a native Vulkan homebrew title (PPSA99142).
- Cover-wheel game library: box art, A–Z letter jumps, Favourites, Recently played, On this console, On the NAS;
  game cards with year, developer, publisher and time played.
- Games on the console, a USB drive or a NAS over NFS (v3/v4, found on the network); `.chd`, `.cue`/`.bin`,
  `.iso` and `.7z`; multi-disc games.
- Settings on the console for the BIOS (it can find `cdimono1.zip` on the NAS), the NAS, covers, DNS and controls.
- Fast start: the player's start-up skipped, Play CD-i clicked automatically once learned, Continue.
- Picture filters: Default, Sharp, Smooth, Enhanced, FSR, Pixel art, Scanlines, CRT; 4:3 / 16:9, crop, frame
  blending.
- Save states, rewind, NAS save backup and restore.
- Remappable face buttons, per-game pointer speed and relative/absolute aiming, a second controller.
- Digital Video (FMV) games with the DVC ROM.
- Covers downloaded from libretro-thumbnails.
