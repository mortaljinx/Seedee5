# Seedee5 developer notes

The project's working diary, kept for whoever builds or changes it: how each part came to be, in the order it was
built. The app was called "CD-i Wheel" until 1.00; the desktop host stage was its first step. Some features
described here (motion aiming, the touch-pad pointer, USB mouse and light guns) were tried during development and
taken out before 1.00 because they did not work on the console. For using the app, see the README and INSTALL.txt.

## ps5-cdi (desktop host stage)

A minimal libretro host (SDL2). Same host logic will later be rebuilt for the
PS5 payload SDK; only window/audio/input glue changes.

## 1. Check the host with the dummy core (no BIOS, no games)

    sudo apt install build-essential libsdl2-dev curl
    make
    ./cdihost ./testcore.so anyfile.bin      # gradient + 440 Hz tone
    make test                                # headless smoke test

Hold Z to tint the picture (input test). F5/F7 saves/loads a state.
Window title shows FPS, audio buffer and underruns.

## 2. Build the real core

    ./scripts/build_core.sh                  # produces same_cdi_libretro.so

## 3. BIOS (you supply it, nothing is bundled)

    system/same_cdi/bios/cdimono1.zip

The libretro docs list cdi200.rom, cdi220.rom, cdi220b.rom and the servo/slave
ROMs as the contents of cdimono1.zip. DVC (later) adds vmpega.rom inside it.

## 4. Run a game

    ./cdihost ./same_cdi_libretro.so /path/to/game.chd --device mouse

Supported by the core: .chd .iso .cue (.m3u for multi-disc).

## Controls

arrows = d-pad, Z/X/A/S = B/A/Y/X, Q/W = L/R, Enter = Start, RShift = Select
F2 mouse capture, F5/F7 save/load state, F10 reset, F11 fullscreen, Esc quit
Gamepad: face buttons, d-pad, shoulders; right stick moves the pointer.

## Smoke-test checklist for the real core

1. Boots to the CD-i player screen (if not: BIOS path/zip name).
2. A standard disc starts; picture has the right aspect.
3. Audio: underruns stay at 0 after the first second.
4. Pointer moves with mouse (F2) and right stick; buttons register.
5. F5 then F7 restores state (core advertises deterministic states).
6. Note any log lines tagged [core ERROR] and send them over.

## PS5 build (Phase 2: test pattern + tone on the console)

Prerequisites (one-off): ps5-payload-sdk installed in /opt/ps5-payload-sdk,
libc++ built with the SDK (libcxx.sh), and the PS5 SDL2 port
(github.com/ps5-payload-dev/SDL, branch release-2.30.x-ps5) installed in
~/ps5-sdl. The test ELF links the dummy test core statically, so it needs no
BIOS and no game.

    export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
    make -C platform/ps5                              # builds cdihost-test.elf
    PS5_HOST=<console ip> make -C platform/ps5 test   # sends it to your ELF loader (port 9021)

Expected on the console: a scrolling gradient on the TV and a steady tone for
about 30 seconds, then the app exits by itself. The text printed in your
terminal shows the SDL video driver name and the frame/underrun counts.
Defaults on the console: /data/ps5-cdi/system and /data/ps5-cdi/saves.

## Phase 3: the real CD-i core on the PS5 (work in progress)

    export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
    ./scripts/build_core_ps5.sh            # compiles SAME_CDI with the PS5 compiler (long)
    make -C platform/ps5 cdihost.elf       # links host + core into one ELF

The build script prints the first compile errors and lists the static
archives it produced. The libretro Makefile's last step (a shared-library
link) is expected to fail on the PS5 toolchain; only the lib*.a archives matter.
On the console the ELF reads /data/ps5-cdi/game.cue and expects the BIOS in
/data/ps5-cdi/system/same_cdi/bios/ (cdimono1 set).

## Saved games (NVRAM)

The CD-i keeps saved games in battery-backed NVRAM. The core writes it to
<save dir>/same_cdi/nvram/<disc name>/ (on the console:
/data/ps5-cdi/saves/same_cdi/nvram/game/ while the disc is called game.cue).
MAME only writes it on a clean shutdown, so patches/0001-flush-nvram.patch adds
retro_flush_nvram() to the core and the host calls it every 20 seconds
(--nvram-interval S to change, 0 = only at exit), when quitting, and when the
system sends SIGTERM/SIGINT/SIGHUP.

    ./scripts/apply_core_patches.sh     # apply the patch to third_party/same_cdi*
    ./scripts/build_core_ps5.sh         # rebuild (only libretro.cpp recompiles)
    make -C platform/ps5 cdihost.elf

Pad: L1 + R1 + Square saves NVRAM immediately; L1 + R1 + Options quits.

## Game library (cover launcher)

With no arguments the PS5 build shows a grid of covers. It scans
/data/ps5-cdi/roms (two folder levels deep) for .cue, .chd and .iso files; the
.bin tracks of a .cue are not listed. Games are sorted by name, the game played
last is preselected, and the title is the file name without its extension
(underscores become spaces).

    /data/ps5-cdi/roms/TheApprentice/TheApprentice.cue   (+ its .bin tracks)
    /data/ps5-cdi/roms/Mad_Dog_McCree.chd

Covers are .png, .jpg or .bmp files (any shape; they are shown without
stretching). Looked up in this order, first match wins:

    covers/<file name>.png          /data/ps5-cdi/covers, exact file name
    <game folder>/<file name>.png   next to the game
    same title with case, '_' and (tags) ignored, in covers/ or next to the game
    <game folder>/cover.jpg         also folder, front, box - only for a folder that
                                    holds a single game

Games without a readable cover get a tile with their title. Covers are decoded
on a background thread (stb_image, public domain) and cached in memory.

Pad: D-pad or left stick = move (hold to repeat), L1/R1 = page, Cross = play,
Square = switch between covers and a plain list, Options = quit. If the roms
folder is empty but /data/ps5-cdi/game.cue exists, that single game is started
as before.

Saved games (NVRAM) are kept per game, named after the file:
/data/ps5-cdi/saves/same_cdi/nvram/<file name without extension>/

Desktop: `./cdihost core.so - --roms DIR --covers DIR` shows the same grid;
`--list` prints the games found and the cover each one matched, then exits.
Passing a game path instead of `-` skips the launcher. To play another game,
quit and start the app again.

Third-party code: host/stb_image.h (public domain / MIT), host/font8x16.h
(glyphs from Terminus, SIL Open Font License 1.1).

## Fetching covers automatically

    python3 tools/fetch_covers.py --ftp <console-ip>:2121 --dry-run   # see what would be fetched
    python3 tools/fetch_covers.py --ftp <console-ip>:2121             # fetch and upload
    python3 tools/fetch_covers.py --roms ~/games --covers ./covers    # local folders instead

The tool reads the game list from the console (or a local folder), matches each
game file name to the Philips CD-i box art set of the libretro-thumbnails project
(ignoring case, underscores, "(USA)" style tags and "Title, The" word order),
downloads the pictures and stores them as <game file name>.png in
/data/ps5-cdi/covers. Lines: "ok" = confident match, "??" = probable match (check
it), "--" = no match (closest names are listed). Region order defaults to
Europe, USA, World (--prefer). Covers that already exist are skipped (--force
to replace). The list of available pictures is cached for 30 days.

## Automatic cover download

When the launcher opens, games without a cover are matched against the box art
list of the libretro-thumbnails "Philips - CD-i" set (host/cdi_boxarts.h: 340 file
names and their SHA-1 ids, no pictures) and the pictures are downloaded in the background, nearest the
selection first. They are saved as /data/ps5-cdi/covers/<game file name>.png, so
the next start finds them on disk and needs no network. The header shows
"Downloading covers  3 / 11" while it works.

On the console this uses the system's own libSceNet / libSceSsl / libSceHttp2
(the same calls as the payload SDK's http2_get sample; PS5SX2 downloads its covers
the same way). Every request has timeouts, and the worker stops for the session when
no server can be reached, so an offline console is not slowed down. A 404 is
remembered for 14 days (covers/.missing/).

Two ways to get a picture, tried in this order:
1. The console's own HTTPS (libSceHttp2/libSceSsl), with proper certificate checks.
2. Our own small HTTPS client (host/tlsget.c: BSD sockets + BearSSL). In a homebrew
   process the console's HTTPS can fail outright (it fails with error 0x8095f00c and
   errno 2), while the console's name resolver and plain sockets work. This client does
   not validate certificates, which is safe because host/cdi_boxarts.h holds the git blob
   id (SHA-1) of every picture: over this path a picture is kept only if it is exactly
   the file in the list, anything else is thrown away. (Over a checked connection a
   picture that differs is kept: the repository may have been updated since the list was
   made.) To forbid the unchecked path, create an empty file /data/ps5-cdi/cover_secure_only.

Two servers are tried for each way: raw.githubusercontent.com, then thumbnails.libretro.com.
Any HTTP answer other than 200 or 404 (a filter's block page, for instance) counts as a
failure and moves on. Everything is written to /data/ps5-cdi/saves/cover_log.txt: the
network state, what the console's resolver answers for each server name, and the result of
every try. Triangle in the launcher tries again.

BearSSL (host/third_party/bearssl, MIT licence, the earlephilhower/bearssl-esp8266 fork
of upstream) is built into build/libbearssl.a by the Makefiles. One local change: the
unused #include <alloca.h> was removed from src/inner.h (the console's C library has no
alloca.h). tools/tlsget_test.c is a small test program for the client.

Switch it off: create an empty file /data/ps5-cdi/no_cover_download (desktop:
--no-download). Only the request for each picture goes out. Matching uses the same
rules as tools/fetch_covers.py (a C port; `./cdihost core.so - --match "Game name"`
prints what a name matches). To refresh the bundled list when new box art is added
upstream: python3 tools/gen_cover_index.py

## A game card's extras (1.20)

When a card opens, the engine (engine/cdi_engine.c, "a game card's extras") fetches, on one worker thread, for
that game only:

- **About and reviews** from Wikipedia's API (`en.wikipedia.org/w/api.php`, one query for the lead's plain text,
  the short description and the wikitext). The list's title is tried, then the file's, then a search for
  "<title> CD-i"; a page is used only if host/gameinfo.c's `gi_is_game` says it is about a video game. Review
  scores come from the `{{Video game reviews}}` table. The card credits the article (CC BY-SA 4.0).
- **A screenshot and a logo** from libretro-thumbnails (Named_Snaps, Named_Logos), matched like the covers and
  checked against the git blob ids in host/cdi_extras.h. Many logos are git symbolic links to another region's
  logo; the list stores each entry's own name (to match) with the file it points to (to fetch).
  Regenerate with `python3 tools/gen_extras_index.py`.

Everything is cached in `data/covers/extras/{info,snaps,logos}`; a miss is remembered for 14 days. With
downloads off in Settings only the cache is read.

Tests: `tools/gameinfo_test.c` (the parsing, with a fuzz pass; build it with ASan) and `tools/extras_test.c`
(end to end, with `tools/fake_wikipedia.py` standing in for Wikipedia):

    make extras_test
    python3 tools/fake_wikipedia.py /tmp/fakewiki &
    CDIHOST_TEST_URLMAP='https://en.wikipedia.org/|https://127.0.0.1:8443/' ./extras_test /tmp/extras-cache

## Digital video (FMV) games

Some games (Mad Dog McCree, Caesars World of Boxing, The 7th Guest...) need the CD-i
Digital Video Cartridge, which plays MPEG video. The core always emulates the cartridge,
but its driver ROM, vmpega.rom, is optional and not part of the usual BIOS set. Without
it those games stop with "digital video capability missing" (the app also says so at
start: "vmpega.rom not found next to the BIOS"). Put the file in the BIOS set, next to
cdi200.rom, in either form:

    /data/ps5-cdi/system/same_cdi/bios/cdimono1/vmpega.rom      (folder)
    /data/ps5-cdi/system/same_cdi/bios/cdimono1.zip              (as an extra file in the zip)

The ROM comes from a CD-i digital video cartridge or from the CD-i 490 ROM set. The core
expects the 256 KB file with CRC32 db264e8b (SHA-1 be407fbc102f1731a0862554855e963e5a47c17b).

## Core options (video standard and others)

Plain text files with one key=value per line (# starts a comment):

    /data/ps5-cdi/options.cfg                  for every game
    /data/ps5-cdi/options/<game file name>.cfg for one game; wins over options.cfg

    same_cdi_video_standard=ntsc               pal (default) or ntsc
    same_cdi_mouse_enable=enabled              (already on)

With no explicit video standard, a game whose file name has (USA) but not (Europe) runs as
NTSC; everything else as PAL. Desktop: --options DIR (default the current folder).

## Sound delay

Sound is played from a buffer that the host tops up one video frame at a time. Its low-water
mark is 30 ms by default (it was 60 ms in earlier builds), with a 512-sample device buffer.
Lower = the sound arrives earlier relative to the picture, but below about 20 ms (one PAL
frame) it can run dry: the perf report's "audio underruns" counts that.

    Pad:  L2 + R1 + D-pad left / right   latency -5 ms / +5 ms (20 to 150), saved in
                                         /data/ps5-cdi/saves/audio.cfg
    options.cfg:  host_audio_latency_ms=30      host_audio_samples=512   (256, 512 or 1024)

A saved pad adjustment wins over options.cfg. Your TV adds delay of its own (turn on its
game mode); if the sound is too EARLY instead, raise the number.

## Pointer: speed and aiming modes (shooters)

The CD-i pointer is a mouse. Two ways to drive it from the pad (Triangle switches, with L2 + R1):

* RELATIVE (default): either stick pushes the pointer along, on a curve (gentle near the centre,
  fast at full tilt: 28 counts per frame at 100 %; fractions are carried over). The D-pad, which
  the console also reports as arrow keys, starts at 8 counts per frame and triples over half a
  second of holding.
* ABSOLUTE: the stick position IS the pointer position, like a light gun. Tilt right and the
  crosshair is at the right edge in one frame; release and it returns to the centre. The CD-i
  keeps an absolute pointer position (768 x 560 units) inside the player; the host tracks where
  it believes that is and sends the difference to the target. Pushing the stick to an edge
  overshoots on purpose, which re-synchronises that estimate if the game has moved the pointer
  itself. The D-pad does not move the pointer in this mode.

    Pad:  L2 + R1 + Triangle           switch relative / absolute
          L2 + R1 + D-pad up / down    speed +10 % / -10 % (30 to 800): in relative mode the
                                       pointer speed, in absolute mode the gain (200 % = the
                                       edge is reached at half tilt)
    Saved in /data/ps5-cdi/saves/pointer.cfg. options.cfg (or options/<game>.cfg) can set a
    default per game:
          host_pointer_mode=absolute        (or relative)
          host_pointer_speed=100
          host_pointer_y_range=560          (try 280 if the pointer reaches the bottom too early)

Buttons: Cross or R2 = CD-i button 1 (fire), Square = button 2.

## Input readout (finding out what the pad and the pointer are doing)

    Pad:  L2 + R1 + Square    show / hide a live readout in the top-left corner of the picture

It lists the raw values the console reports for both sticks, both triggers and the buttons, whether
the D-pad arrives as arrow keys, how many pointer counts were handed to the core in the last frame
and in total, and where the host believes the pointer is. A magenta cross marks that position on the
picture: compare it with the game's own crosshair. If the cross moves but the game's crosshair does
not follow, the limit is inside the game; if the stick values stay at 0, the pad is not reporting
them to the app.

## Why the pad could not move the pointer (core patch 0002)

In this core the CD-i's pointer ports (MOUSEX / MOUSEY of the slave) end up with an empty input
assignment, so MAME never reads the mouse device for them and every pointer count the host handed
over was dropped. The only thing that moved the pointer was the D-pad, as buttons: a fixed 2 units
per frame, whatever the speed setting. patches/0002-cdi-pointer-counts.patch hands the counts to
those two ports directly (and makes MAME treat a value written to a relative port as that frame's
movement). Checked against the real core: a stick at full tilt now arrives as 28 units per frame
(100 %) or 84 (300 %), and the absolute mode puts the pointer at the stick position in one frame.

    ./scripts/apply_core_patches.sh      # applies 0001 (NVRAM) and 0002 (pointer)
    ./scripts/build_core_ps5.sh          # rebuilds; only three core files recompile

The D-pad is no longer handed to the core as buttons (the host moves the pointer itself, with the
speed setting). options.cfg: host_dpad_digital=1 brings the old behaviour back.

## Link note: a second libSDL2.a in the SDK folder

The SDK's compiler wrapper (bin/prospero-clang++) puts target/lib and target/user/homebrew/lib
in front of every -L the Makefile gives. If another libSDL2.a sits in
/opt/ps5-payload-sdk/target/user/homebrew/lib (for instance an SDL installed with the SDK's
default prefix), it silently replaces ours, and a build of it that refers to GNU libiconv fails to
link with "undefined symbol: libiconv_open". platform/ps5/Makefile therefore names our library by
its full path ($(SDL)/lib/libSDL2.a) instead of -L/-lSDL2. To see which one the linker uses:
    make -C platform/ps5 cdihost.elf 2>&1 | grep "in archive"

## Pointer feel (relative mode)

The stick curve was made gentler in v29: dead zone 8 % of the stick value, a softer curve and a top
speed of 14 counts per frame at 100 % (it was 28). The console's own driver already shapes the stick
(about 50 % tilt reads as 49 % value, 70 % as 78 %, saturating by 80 %), which is why the top of the
range felt abrupt.

    Hold L1 (alone)                     slow pointer to 40 % while held: fine aiming
    L2 + R1 + D-pad up / down           speed +-10 % (30 to 800), saved in saves/pointer.cfg
    L2 + R1 + R3                        speed back to 100 %
    options.cfg                         host_pointer_speed=NN

A speed you set while testing is saved and applied at every start, so a value raised earlier stays.
The start-up notification shows it ("pointer relative NN%").

## Games on a NAS (NFS network mount)

The launcher lists the games on an NFS share and copies the one you start to the console. It uses libnfs
(LGPL-2.1+, user space, plain sockets): nothing is mounted into the console's file system, so no mount rights
are needed, and the share is read in the background, so the launcher opens at once even if the NAS is off.
The line under the title says what is going on ("NAS: connecting ...", "NAS: 12 game file(s) found", or why
it failed), and everything is written to **/data/ps5-cdi/saves/nas_log.txt**. L3 reads the share again.

**1. Build libnfs for the console once** (needs git and cmake; the version is pinned, and a small patch of ours is
applied: connect from a reserved source port, which many NAS boxes insist on unless exported "insecure"):

    export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
    ./scripts/build_libnfs_ps5.sh
    make -C platform/ps5 clean && make -C platform/ps5 cdihost.elf     # must print "NAS support: libnfs found ..."

(If you built it before v34, run the script again to get the patch.) Without build/libnfs-ps5 the program is built
without NAS support. Desktop (testing): `apt install libnfs-dev`, or `make NFS_DIR=<prefix of a libnfs install>`.

**2. On the NAS, export the games folder over NFS** to the console's IP address (read-only is enough).
Linux / most NAS boxes, /etc/exports style:

    /srv/games  192.168.1.0/24(ro,all_squash,no_subtree_check,insecure)

`insecure` lets clients use ports above 1023; with our reserved-port patch it is not needed, but it does no harm.
Synology: Control Panel > Shared Folder > NFS Permissions: add the console's IP, squash "Map all users to admin"
or "No mapping", allow non-privileged ports. TrueNAS / Unraid / OpenMediaVault: NFS share for the console's IP.
NFS version 3 is tried first, then 4 (a NAS that only offers 4 works too).

**3. Create /data/ps5-cdi/nfs.cfg on the console:**

    server=192.168.1.20          # IPv4 address or name of the NAS
    export=/volume1/games        # the export, exactly as `showmount -e 192.168.1.20` lists it
    path=cdi                     # folder inside the export to search (optional; 3 levels deep)
    cache_gb=10                  # space copied games may use on the console (default 10)
    cache_dir=/data/ps5-cdi/cache

Optional lines: `uid=1000` and `gid=1000` (the user to act as, if the NAS maps users); `version=3` or `version=4`
(only that one; default: 3, then 4); `nfsport=2049` and `mountport=20048` (fixed ports instead of asking the NAS's
portmapper on port 111, for a firewall that only opens NFS itself); `reserved_port=0` (do not use a reserved source
port); `timeout=10` (seconds to wait for the NAS, 2 to 60).

**What the status line and nas_log.txt tell you**

| It says | Usually means |
|---|---|
| no answer (NAS off, wrong address, or a firewall) | nothing at that address answers on port 2049: wrong `server=`, NAS asleep, or a firewall dropping packets |
| refused (NFS not running there, or blocked) | the NAS is there but the NFS service is off, or a firewall rejects port 2049 |
| the export refuses this console | MNT3ERR_ACCES: the console's IP is not allowed on that export, or the NAS wants a reserved port (see above) |
| export path not found | `export=` is not what `showmount -e` lists (it must match exactly) |
| NFS version not offered | the NAS has no NFS v3: set `version=4` |
| mounted, but the folder 'x' is not there | `path=` is wrong; it is relative to the export, with no leading slash |

From a laptop you can check the NAS the same way the console talks to it:
`showmount -e <nas-ip>` lists the exports, `rpcinfo -p <nas-ip>` shows the NFS and mount services.

**Playing.** The games found (.cue, .chd, .iso, .7z) are listed with the local ones, tagged **NAS**; a game that exists
locally with the same file name wins. Starting a NAS game copies it to cache_dir with a progress screen (Circle
cancels). A CHD needs random access, which a network share cannot give the core, so there is no streaming. For a
.cue the files it names (FILE "...") are copied too. A complete copy is reused the next time; a half-finished copy
resumes at the file it stopped in. When the cache would pass cache_gb, the games played longest ago are deleted
first. If a copy fails you get the reason and return to the list.

Covers for NAS games work as for local ones (downloaded automatically, or in the covers folder).

## Packed games (.7z)

A game can be a single .7z file instead of a .cue with its tracks or a .chd. Put it in the roms folder
(or on the NAS, see above) next to the others. It is listed with a "7z" tag (or "NAS" if it lives on
the NAS). Starting it unpacks it into the cache folder (/data/ps5-cdi/cache, or cache_dir from nfs.cfg)
with a progress screen (Circle cancels), then starts the .cue inside (or, if there is none, a .chd or
.iso). A NAS archive is copied first and the copy is deleted again after unpacking.

* An archive that is already unpacked is reused. It is unpacked again if the .7z changes (size or time).
* Unpacking is streamed: a 700 MB disc image needs only the LZMA dictionary in memory, not 700 MB.
* Every file is checked against its CRC; a damaged archive stops with a message and leaves nothing behind.
* Supported compression: **LZMA2 and LZMA** (7-Zip's default), solid or not. PPMd, BZip2, filters (BCJ,
  Delta), encryption and multi-volume archives are refused with a message. To make a suitable archive:
      7z a -t7z -m0=lzma2 -mx=5 "Game (Europe).7z" "Game.cue" "Game (Track 01).bin" ...
  (Disc images do not compress much: -mx=1 is faster to make and about as small.)
* A .7z, a .cue and a .chd with the same file name: the first one found wins; keep one per game.
* The unpacked games share the cache with copies from the NAS: the games played longest ago are deleted
  first when the cache would pass cache_gb (default 10 GB), also for .7z files without any NAS set up.
  cache_gb and cache_dir are read from nfs.cfg whether or not a NAS is configured.

The 7z reader is the LZMA SDK from 7-Zip (public domain, host/third_party/lzma), unchanged. tools/arc7z_test.c
unpacks an archive from the command line for testing.

## Buttons (v32): what the CD-i actually has

The emulated CD-i player has **two buttons**, "button 1" and "button 2" (a third is ignored by the
core). In this core they are read from the RetroPad's **Y** (button 1) and **B** (button 2); mouse
clicks are dropped. Before v32 the pad sent Cross as B and Square as A, so button 1 could not be
pressed at all, which is why games that select with button 1 (character selection, menus) seemed dead.
Now the pad's buttons are mapped to the CD-i's two buttons like this (readout: L2 + R1 + Square shows
"CD-i buttons: 1[X] 2[ ]" while you hold them):

    Cross = button 1     Circle = button 2     Square = button 1     Triangle = button 2     R2 = button 1
    L1, R1, L2: nothing (they are used in shortcuts)

Change it for one game in options/<game name>.cfg (the .7z's own name for packed games), or for all
games in options.cfg. Each value is 1, 2, 12 (both buttons together) or none:

    host_button_cross=2
    host_button_circle=1
    host_button_triangle=12
    # also: host_button_square, host_button_l1, host_button_r1, host_button_l2, host_button_r2

## Pointer settings are per game (v32)

The pointer speed and mode (relative / absolute) that you set with L2 + R1 + D-pad up/down and L2 + R1 +
Triangle are saved per game in saves/pointer/<game name>.cfg, so absolute aiming set up for a shooter
does not follow you into a racing game (absolute mode recentres the pointer whenever the stick is
released, which games that use pointer movement as a direction read as input). An older saves/pointer.cfg
still supplies the speed for games without their own file, but no longer the mode. The start of every
game is logged: "[host] settings for '<game>': pointer relative 100%, buttons cross=1 ...".

## Smooth motion (v33): even frame pacing and video smoothing

Two separate problems made fast games (Micro Machines) look choppy even though the performance report
showed a steady 50 fps.

1. **Uneven frame times.** Frames used to be made whenever the audio buffer dipped below a mark. The audio
   device takes sound in chunks (512 samples = 10.7 ms at 48 kHz), which do not divide a 20 ms frame evenly,
   so frames came 11 and 22 ms apart instead of 20. Frames are now scheduled by the clock, evenly, and the
   audio buffer only steers that clock by a fraction of a percent (up to 3 % if the audio device runs off
   its nominal rate; a safety net makes a frame at once if the sound is about to run dry). Measured with a
   chunk length that does not divide the frame length: spacing spread (std dev) 7.0 ms before, 0.5 ms now.
   `host_audio_latency_ms` keeps its meaning (lower = sound earlier); the buffer now aims 1.5 chunks above it
   to stay clear of the swing a chunk causes.

2. **50 pictures a second on a 60 Hz screen.** The console always shows 60 pictures a second; a PAL CD-i makes
   50, so every sixth picture is shown twice and scrolling stutters. With smoothing the picture at each refresh
   is a blend of the two newest frames, weighted by where that refresh falls between them. Measured with a
   moving bar: the movement per refresh varies by 0.12 px (std dev) instead of 1.6 px. It costs about one frame
   (20 ms) of extra delay and a slight softness on fast motion. It switches itself off when the frame rate and
   the refresh rate match (an NTSC disc at 60 fps).

       Pad:  L2 + R1 + L3        smoothing on / off (saved per game, in saves/pointer/<game>.cfg)
       options.cfg:  host_smooth_video=0 or 1      host_smooth_strength=0..100  (how much of the blend is used)

The performance report now ends with "early frames +N": frames made ahead of schedule by the safety net (it should stay at 0 or a few; a steady stream means the sound is starving).

Because the picture is now presented at every refresh, the performance report's "screen" figure reads about
60 fps with smoothing on, and the render thread works harder (about 60 % of one core at 1080p).

## Games on a USB / external drive (v34)

Plug the drive into the console (exFAT or FAT32). It appears as /mnt/usb0 ... /mnt/usb7 (extended storage as
/mnt/ext0, /mnt/ext1). On each drive the first of these folders that exists is searched, three levels deep:

    ps5-cdi/roms   ps5-cdi   cdi   CDi   CD-i   roms/cdi   roms

Games found there (.cue, .chd, .iso, .7z) are listed with the others, tagged **USB** (a .7z keeps its "7z"
tag). They are played from the drive itself, with no copy, because a USB drive is random-access storage; a
.7z is unpacked into the cache folder as usual. A game with the same file name that is already in the roms
folder wins, and a drive with no such folder is skipped (the log says so).

* **L3 in the launcher rescans.** A drive plugged in after the app started is not seen until you press it;
  the cursor stays on the game it was on. The list is also scanned every time the app starts.
* The launcher shows the drive under the title ("usb0: Game.chd").
* Covers next to the games on the drive are used like those in the roms folder; the automatic downloads
  still go to /data/ps5-cdi/covers. Saved games and settings stay in /data/ps5-cdi/saves.
* **/data/ps5-cdi/drives.cfg** (optional, key=value per line, # starts a comment):

      folder=Games/CDi         the folder to look for on each drive; given once or more it replaces the list above
      extra=/mnt/ext0/cdi      another full path to search (up to 8)
      enabled=0                do not look at external drives at all

* To keep unpacked .7z games and NAS copies on a drive too, point the cache there in nfs.cfg
  (cache_dir=/mnt/usb0/ps5-cdi-cache). Mind that a drive pulled out while a game from it is running ends
  that game's disc reads.
* On a desktop build the same scan looks at /media/<user>/<name> and /run/media/<user>/<name>.

## Launcher keys (v34)

    Cross: play    D-pad / left stick: choose    L1 / R1: page    Square: covers / list
    L3: rescan (local folders, USB drives, and the NAS again)    Triangle: retry cover downloads    Options: quit

## Big libraries (v35)

Tested with a 2020-game list: scanning takes a fraction of a second and the launcher opens at once.

* **L2 / R2 jump to the previous / next letter** (all titles that start with a digit or a symbol are one group;
  past the last letter it wraps to the first). With 10 games per page, L1 / R1 would take over 200 presses.
  Square switches to the plain list view, which shows more at once.
* **Cover matching** runs in the background thread now (it used to run while the launcher opened) and has
  stricter rules, because a wrong picture is worse than none:
  - titles with different numbers are different games (a different year of an award show, "Crime Patrol 2");
  - a game whose name merely *extends* a cover's name ("Mad Dog McCree III", "Crime Patrol Drug Wars") is
    treated as a sequel and does not borrow the cover; the other way round still matches
    ("Zombie Dinos" gets "Zombie Dinos from Planet Zeltoid").
  Regional variants of the same game ("(Germany)", "(USA)") still get the cover of the game. With a typical
  full set about a sixth of the games have no box art in the source collection (prototypes, educational
  discs, regional releases), so they keep a title tile. tools/fetch_covers.py follows the same rules.

## Covers for games on the NAS, and a library that is all on the NAS (v36)

* **NAS games get covers exactly like local games**: the same matching against the libretro-thumbnails box art
  list and the same background download into /data/ps5-cdi/covers (saved under the game's file name). Tested
  with a launcher whose list came only from the NAS: the covers downloaded and appeared on the NAS tiles.
* **The source has box art for only 340 games** (343 names with any picture of any kind; its title screens and
  snapshots add three more), so of a full set of about 2000 files roughly a sixth get a cover and the rest keep a
  title tile. That is the limit of the source, not of the NAS. For other games, put your own pictures on the
  console in /data/ps5-cdi/covers named like the game file (Game Name (Europe).png or .jpg); they are used before
  anything is downloaded.
* **Fixed: an empty roms folder.** With no local games the launcher used to decide there was nothing to show before
  the NAS list had arrived and quit. It now opens, shows "No games found" and the NAS status, and fills in when the
  scan finishes.
* **Mistyped or hidden NAS folder.** When `path=` cannot be opened, nas_log.txt now walks the path one folder at a
  time and lists what each level holds ("top of the share (2 entries): philips/, cdi/", then "'philips' (1 entries):
  cd-i-chd/") plus the exact NFS error, and the launcher says which part is missing or not permitted.
* Mount the NAS itself, not a server that mounts the NAS: a Linux server cannot re-export an NFS-mounted folder
  reliably, and the share may then look empty or stale to the console.

## In-game menu (v37)

Press the **touchpad** while playing (Tab on a keyboard) to pause the game and open the menu. The sound stops while it
is open; Circle or the touchpad resumes. The D-pad or left stick moves, Left / Right change a value, Cross chooses.

| Line | What it does |
|---|---|
| Resume | back to the game |
| Save state  < slot N > | saves the whole machine into slot 1-4 of this game (Left/Right picks the slot; the line shows when it was saved) |
| Load state  < slot N > | restores that slot; if it was saved with another disc in the drive, that disc goes back in first |
| Disc  < insert 2 of 2 > | only for games on more than one disc: pick a disc and press Cross to put it in the drive |
| Pointer | relative, or absolute (aim: the stick position is the pointer position) |
| Pointer speed | 30 to 800 % |
| Smooth video | on / off |
| Sound delay | 20 to 150 ms |
| Screen | 4:3 or 16:9 (fill) |
| Reset game | restarts the game (saved games in NVRAM are kept) |
| Back to the game list | unloads this game and shows the list again, so you can pick another one without restarting the app |
| Quit | ends the app |

Changes are saved like the pad shortcuts (pointer and smoothing per game, the rest globally). Save states go to
/data/ps5-cdi/saves/states/<game>.slotN.state (plus .disc for multi-disc games). The button that closes the menu is
not passed on to the game: the pad is ignored until it is released. The touchpad is no longer handed to the core.

## Games on more than one disc (v37)

Files named "Game (Europe) (Disc 1)...", "(Disc 2)..." in the same folder (local, USB or on the NAS) are listed once,
as "Game (Europe)". Starting it hands the core a disc list (.m3u, written to saves/discs/ for local games or next to
the copy for NAS games, which copies every disc); disc 1 goes in first and the menu's Disc line swaps. A .7z holding
several "(Disc N)" images gets a disc list too after unpacking (separate .7z files per disc are not grouped). Saved
games (NVRAM) of a multi-disc game are kept under the name of the disc list, so they are separate from any you made
before by starting disc 1 on its own.

## Back to the game list fixed (v38)

In v37, a game started after "Back to the game list" stayed frozen on its first picture: the SAME_CDI core cannot run
a second game after it has unloaded one. The host now shuts the core down and starts it again between games
(retro_deinit, retro_init), which also gives the next game its disc tray back.

## CD-i Wheel: the native Vulkan title (v39, first build)

CD-i Wheel is ps5-cdi as a native PS5 title (PPSA99142) on the PS5 Vulkan Template, like Jellyfin Wheel: the game
list is the UI kit's Cover Wheel, the game is drawn by the GPU through RADV, and its sound goes out with the kit's.
The SDL payload (cdihost) stays as it is, as the fallback. Both use the same folder, /data/ps5-cdi (roms, covers,
saves, system, nfs.cfg, options), so games, covers, saves and settings are shared.

What is different from the payload:
- the list: spin with the left stick (it coasts and snaps), D-pad steps, L1 / R1 jump to the previous / next letter,
  L2 / R2 jump ten games, Cross opens a game's card, Cross again plays it, Triangle marks a favourite (kept in
  saves/favourites.txt), Square looks for games again, hold OPTIONS for a second to close the title;
- a game on the NAS or in a .7z shows a progress card while it is copied or unpacked (Circle cancels);
- the touchpad menu has the same lines as the payload's, plus Crop and "Fit the crop to the picture";
- smooth video is done on the GPU: the two newest frames are two images, the newest drawn over the previous at the
  engine's blend;
- games on USB drives are not seen: a title's sandbox hides /mnt (put them on the NAS or in /data/ps5-cdi/roms);
- covers are downloaded with names looked up by the title itself (a plain DNS query to 192.168.1.1, then 1.1.1.1 and
  8.8.8.8, or to the address in /data/ps5-cdi/dns.txt), since a title cannot use the system's resolver;
- the log is /data/ps5-cdi/saves/title_log.txt (and klog), started again at every launch.

The engine (engine/cdie.h, engine/cdi_engine.c) is everything but drawing, taken from host/main.c: the list, covers,
NAS, 7z, the core on its own thread with the payload's pacing, pointer, buttons, save states, discs and settings.
tools/cdie_sdl.c is its desktop test program (`make cdie_sdl`).

### Making and building it (on the laptop, next to Jellyfin Wheel)

The template, PS5_Vulkan, PS5_PayloadSDK and PS5_VKHomebrewUI go in one folder, here ~/ps5 (any folder works), with the title beside the
template (~/ps5/Seedee5 next to ~/ps5/PS5_VulkanTemplate).

    export PATH=~/ps5/venv/bin:$PATH
    export CCACHE_DIR=~/ps5/ccache
    unset PS5_VULKAN_DIR PS5_PAYLOAD_SDK
    cd ~/Documents/ps5-cdi
    python3 title/make_cdi_title.py ~/ps5/Seedee5          # once: makes the title
    scripts/build_title_libs.sh ~/ps5/Seedee5              # the core and libraries (long the first time)
    cd ~/ps5/Seedee5
    ps5/tools/build.sh && ps5/tools/deploy.sh --all

Wait about 45 seconds after the first deploy before launching (the console has to notice the new title folder),
then start Seedee5 from the home screen. Watch klog with `nc <console-ip> 3232 | tee ~/klog-cdi-1.txt`.
After a new ps5-cdi version: `python3 title/make_cdi_title.py ~/ps5/Seedee5 --update`, then
scripts/build_title_libs.sh again only if the core or the patches changed, then build.sh and deploy.sh.

build_title_libs.sh builds the core in its own tree (third_party/same_cdi_title) with the title's SDK, which is not
the payload's: the two sets of libraries must not be mixed. On the PC, `scripts/build_title_libs.sh --host` builds
the same set for the template's host reference build (ps5/tools/host-reference.sh; with -DCDI_LIBS_DIR pointing at
build/title-libs-host, and CDI_DATA_DIR for the data folder).

## Settings for everyone (v43): NAS, BIOS and games set up from the console

CD-i Wheel now sets itself up for whoever installs it; nobody needs to write nfs.cfg by hand.

* **First launch** makes /data/ps5-cdi with roms, covers, saves and system/same_cdi/bios, plus a "READ ME.txt"
  saying what goes where. The settings open by themselves when there are no games and no NAS yet, or no BIOS.
* **OPTIONS, a short press** on the wheel opens the settings (holding OPTIONS still closes the app).
  * **CD-i BIOS**: whether cdimono1.zip (and vmpega.rom for FMV games) is there, where to put it over FTP, and
    **Find the BIOS on my NAS**: searches the share (six folders deep) and copies the one you pick in.
  * **NAS**: **Find NAS on my network** asks every address on the console's network for NFS and lists the
    servers; or **Enter the address** (four numbers, D-pad, no keyboard). Then pick a share, then browse to the
    folder with the games (each folder says how many game files it holds) and press **Use this folder**: it is
    saved to nfs.cfg and the NAS is read at once. Also: on/off, NFS version, reserved source port, room for
    copied games. Keys the screen does not show (cache_dir and the rest) are kept when it saves.
  * **Games found**: the count, and where to copy games over FTP, multi-disc naming and box art.
  * **Covers**: downloads on/off, try again, and a DNS server for them (automatic = the router).
  * **Look for games again**.
* Starting a game without the BIOS now says so plainly instead of failing inside the core.
* Limits: NAS means NFS (v3 or v4); SMB/Windows shares are not supported. USB drives cannot be used by a
  native title (it cannot see /mnt), so games go in /data or on a NAS.

Update an existing title: `python3 title/make_cdi_title.py ~/ps5/Seedee5 --update`, then
`ps5/tools/build.sh` and `ps5/tools/deploy.sh --all` as before (no library rebuild is needed if v41+ libs
were built, but the engine changed, which the title build compiles itself).

## Fast start (v44): straight into the game

Starting a game used to show the CD-i player's own start-up: its blue screen, a switch of screen mode, then
the game. Now that part runs as fast as the console can emulate it, unseen and unheard, behind a "Starting"
card, and the game appears once it has its own picture: once the screen mode has changed and then stayed
put for 0.8 s (mode changes in the first 2 s are the player's own and do not count), or after 30 s of CD-i
time at most. Cross on the card shows the game at once. Opening the touch pad menu works as before.

The title log (saves/title_log.txt) says what happened, e.g.

    [host] fast start: frame 412 (8.2 s of CD-i time): screen 384x280 (was 768x280), brightness 31
    [host] fast start done (the game's screen mode): 452 frames = 9.0 s of CD-i time in 2.6 s (3.5x), 1 screen mode change(s)

options.cfg (or options/<game>.cfg for one game):

    host_fast_start=0          off: see the player's start-up as before
    host_fast_start_max=30     most CD-i seconds to skip
    host_fast_start_settle=40  frames the game's screen mode must hold before it is shown
    host_fast_start_ignore=100 mode changes before this frame are the player's own

## Settings that could not be saved (v45), NAS port retry (v46)

On some consoles the data folder (/data/ps5-cdi) was made by FTP or by the old payload, as another user, and the
title is not allowed to write in it: "Save, and read the NAS" said "could not write nfs.cfg". The settings screen
now writes nfs.cfg, dns.txt and no_cover_download in the saves folder when that happens (a copy there wins over
the one in the data folder), and the log says so. If saving still fails, the screen shows why.
v46: if the NAS will not mount, the list now also tries it with the reserved source port setting the other way
round (a title may not be allowed one), as the settings screen already did; nas_log.txt shows both answers.

## NAS "getaddrinfo error 4" on the console (v47)

nas_log.txt said: `Invalid address:<nas-ip>. Can not resolv into IPv4/v6 structure. getaddrinfo error 4`.
The link recipe binds getaddrinfo to the platform layer's own (`--defsym getaddrinfo=ps5_getaddrinfo`), which
overrides the title's numeric one, and that failed libnfs's plain IPv4 address with EAI_FAIL. The title build now
links a copy of libnfs.a (build/ps5/libnfs-cdi.a, made with objcopy --redefine-sym) whose calls go to the title's
own cdi_getaddrinfo. Nothing to rebuild by hand: make_cdi_title.py --update, then build.sh. Needs objcopy or
llvm-objcopy on the laptop (binutils).

## Buttons in the game, the exit combo, letters on the wheel (v48)

* Buttons: after a game starts (or the menu closes) the button that did it is still down. Until v47 the whole
  pad was kept from the game until every button read as up; one button or trigger that never read as up kept
  every button from the game, while the pointer (D-pad, sticks) still moved. Now only the buttons that were down
  at that moment are ignored, each until it is let go, and after 3 s they are given back anyway. The log says
  which ones ("[host] pad: ...") and when the first button reaches the game.
* L1 + L2 + R1 + R2 + OPTIONS together, in a game: straight back to the game list.
* The wheel: L1 / R1 jump to the previous / next letter (the big letter shows); held, they keep stepping a
  letter at a time. L2 / R2 jump ten games.

## The engine's own log (v50)

The engine now writes its log itself to saves/cdi_log.txt (started again at each launch), next to nas_log.txt,
as well as handing it to the title (title_log.txt, klog). v49 adds a line for each of the first 40 button changes
in a game: "[host] pad: buttons <from the title> -> core <what the CD-i core gets>".

## Where CD-i Wheel keeps its files (v51)

A title runs in a sandbox: its /data is not the console's /data that FTP shows (a title only reaches that when it
is elevated). From v51 the title keeps everything in its own folder, /app0/data, which FTP sees as

    /data/homebrew/PPSA99142/data/
        roms/  covers/  saves/  system/same_cdi/bios/cdimono1.zip  nfs.cfg  options.cfg  READ ME.txt

On the first start there it copies what it can read from the old payload's /data/ps5-cdi (cdimono1.zip, nfs.cfg,
dns.txt, options.cfg). The logs are saves/title_log.txt and saves/cdi_log.txt in that folder. If /app0 cannot be
written, /data/ps5-cdi is used as before. The SDL payload keeps using /data/ps5-cdi.

## Crash during cover downloads (v52)

The title closed right after "dns raw.githubusercontent.com" (the first cover download). The likeliest cause:
the HTTPS client seeded its random numbers with sceRandomGetRandomNumber, a console library function a native
title links against but may not have loaded. It now uses the CPU's RDRAND (then /dev/urandom). Each step of a
download is logged ("[host] covers: download: connecting / TLS setup / random seed / ..."), so if it still stops,
the last line says where. To run without downloads: Settings > Download missing covers: off, or put a file named
no_cover_download in the data folder.

## Buttons in a game (v53)

The CD-i's buttons are on the face buttons only: Cross and Square = button 1, Circle and Triangle = button 2.
L1, R1, L2, R2 and OPTIONS do nothing in a game (R2 used to be button 1, L1 slowed the pointer, OPTIONS was the
CD-i's start); the L1 + L2 + R1 + R2 + OPTIONS exit of v48 is gone. The touch pad opens the menu (Back to the game
list, Screen 4:3 / 16:9, Crop...). Per game, options.cfg can still change any of it (host_button_square=12 makes
Square both buttons together, for example).

The screen setting (4:3 or 16:9, and the crop) is kept in saves/view.cfg. The payload's one is in
/data/ps5-cdi/saves/view.cfg: copy it into /data/homebrew/PPSA99142/data/saves/ to have the title look the same
(the title copies it itself on its first start when its sandbox lets it read that folder).

## The next game started the previous one's disc (v54, core patch 0003)

After "Back to the game list", the next game booted the CD-i player but its disc was still the previous game's.
The core kept one set of MAME options for the whole session (a static object in retromain.cpp); when a disc is
loaded MAME pegs the disc option above command-line priority, so the next game's "-cdrom <file>" was ignored.
patches/0003-fresh-options-per-game.patch gives every game a fresh set. Checked on the PC: the mounted disc
was the first game's for both before, and each game's own after.

The title needs its core rebuilt once (only one file recompiles):

    cd ~/Documents/ps5-cdi && scripts/build_title_libs.sh ~/ps5/Seedee5

## Picture filters (v55)

The touch pad menu has a Picture setting, kept per game (saves/picture/<game>.txt; the last choice is the default
for games that have none):

| Picture   | What it does |
|-----------|--------------|
| Default   | the frame as it is, scaled smoothly (as before) |
| Sharp     | crisp square pixels without the shimmer of plain nearest-neighbour scaling |
| Smooth    | bicubic (Catmull-Rom): softer, no blockiness |
| Enhanced  | for Digital Video games: an edge-preserving clean-up of MPEG's blocks and ringing, contrast-adaptive sharpening (AMD's FidelityFX CAS idea), both at the source size, then an anti-ringing bicubic upscale |
| Scanlines | the picture's own lines with dark gaps between them |
| CRT       | a TV: beams that widen with brightness, the next line's beam overlapping, an aperture-grille phosphor mask, in linear light |

A CD-i has no 3D scene to draw again at 2x / 3x / 4x, as PS2 emulators do: its pictures are 2D (384x280 or
768x560, video at about 352x288), so these filters work on the finished frame. They run on the GPU as one compute
shader (title/examples/cdiwheel/picture.comp; its SPIR-V is in picture_spv.h, made with
`glslangValidator -V --target-env vulkan1.0 picture.comp --vn cdi_picture_spv -o picture_spv.h`), each new frame
filtered once at the size it is shown at. tools/testcore.c shows a still picture with TESTCORE_IMAGE=<320x240 P6
PPM>, for comparing them on the PC.

## Straight into the game: Play CD-i clicked by itself, Continue (v56)

* **Play CD-i by itself.** The first time a game reaches the CD-i player's start screen, the moves and the click you
  make there are recorded (saves/start_click.txt) and kept once the game's own picture follows. From then on the fast
  start plays them back itself, unseen, and runs on until the game's own picture: choosing any game leads straight to
  its title. The player's start screen is the same for every game, so one recording serves all of them. If a playback
  is not followed by a new picture within 25 s, the start screen is shown and your next click there is recorded
  instead. Delete start_click.txt to record it again; options.cfg host_auto_play=0 turns it off.
* **Each game's start is learned** (saves/start/<game>.txt): where its own picture showed, or where you pressed Cross
  on the Starting card. The next start runs exactly that far.
* **Continue.** Leaving a game you played for 10 s or more saves where it was (states/<game>.slot0.state). Its card
  then offers Continue (Cross: straight back to that moment, no start-up at all) and Start over (Square).

Checked on the PC with tools/testcore.c's pretend player (TESTCORE_SHELL=1: a start-up, a start screen of another
size with a pointer and a Play box): the click recorded on one game started a second game by itself, and Continue
resumed from the automatic save with the real core too (a 5.5 MB state).

## Controls (v57): Settings > Controls

* **Buttons:** what Cross, Circle, Square and Triangle press on the CD-i (button 1, button 2, both, nothing), for
  every game (options.cfg host_button_*; a game's options/<game>.cfg still wins).
* **Touch pad as a trackpad:** a finger sliding on the touch pad moves the pointer (speed adjustable); its click still
  opens the menu.
* **Motion aiming:** turning the controller moves the pointer (the gyroscope; a 30-degree turn crosses the screen at
  100 %), with up and down swappable. Best with Pointer: relative.
* **Second controller:** the next signed-in user's controller moves the same pointer and presses the same buttons.
  The CD-i core emulates one pointing device (MAME's CD-i slave has one), so two players share it rather than having
  one each.
* **USB mouse or light gun:** a mouse's moves and its left/right buttons (button 1/2). Light guns that present
  themselves as a USB mouse with no PC software (GUN4IR, Retro Shooter RS3, Ultimarc AimTrak) should aim the pointer
  this way; not tested on a console, and a gun that reports absolute positions may behave differently.

The touch pad's fingers and the motion sensors come from the same pad reading the template uses
(scePadReadState), and the mouse from libSceMouse; all are looked up when the title runs (title/ps5/src/cdi/cdi_pad.c),
so a firmware without one only loses that feature. The log says which were found ("CDI: pad extras", "CDI: USB mouse").

## The game list (v58): lists, details, time played

* **Lists:** the touch pad on the wheel moves to the next list: every game, Favourites, Recently played (newest
  first), On this console, On the NAS. The header says which; an empty one says how to fill it.
* **Details:** the card shows the year, developer and publisher of games the bundled list knows
  (title/examples/cdiwheel/games_info.hpp: facts from Wikipedia's "List of CD-i games"; data/games_info.txt adds
  or corrects entries, one "Title|Developer|Publisher|Year" a line), and the time played.
* **Time played:** counted from the moment a game's own picture shows until it is left (saves/playtime.txt:
  "<seconds> <last played> <game>").
* The covers are asked for by what the wheel shows (cdie_cover_wanted), so a list keeps its covers loading
  nearest-first even though its neighbours are far apart in the full list.

## Saves on the NAS, rewind (v59)

* **Saves on the NAS:** Settings > NAS > "Back up saves to the NAS after each game" (nfs.cfg backup_saves=1).
  When a game is left (or the app closed from a game), every file in saves/ that is newer than its copy is written
  to <share>/cdi-wheel-saves/: the CD-i's memory with the games' own saved games (same_cdi/nvram), save states and
  Continue, favourites, time played, the settings kept there. "Back up saves now" does it at once; "Bring saves
  back from the NAS" copies back the share's files that are newer than the console's (a new console, a reinstall).
  The share must be writable by the console (NFS export read/write; the NAS's permissions for its folder).
* **Rewind:** the in-game menu's "Rewind (hold L2)" turns it on (saves/rewind.cfg). Every 0.2 s the core's state is
  kept as its difference from the one before (a 5.5 MB state is usually a few KB of difference); holding L2 goes
  back up to 30 s, a step every two frames, silently, with a Rewind mark. Taking a step cost 3.5 ms on the PC; the
  log says what it costs on the console ("[host] rewind: ..."). Off by default.

## Two more pictures (v60): Pixel art and FSR

| Picture   | What it does |
|-----------|--------------|
| Pixel art (xBR) | xBR-style corner rounding: where two diagonal neighbours make an edge, the corner of the texel is cut along it, anti-aliased; square and sharp elsewhere. For the cartoon-style games and menus |
| FSR (video)     | the clean-up of Enhanced, then an edge-adaptive upscale written after AMD FidelityFX Super Resolution 1's EASU (the edge's direction and strength steer a stretched, rotated Lanczos-like kernel over the 12 nearest texels, clamped to the 2x2 around it), then contrast-adaptive sharpening at the output size as FSR 1's RCAS does |

FSR draws diagonal lines and edges of video with fewer jaggies than Enhanced, a little softer; Enhanced keeps more
of the fine texture. Both are in the menu's Picture setting, for each game to keep its own. The shader code is this
project's own (GPL-3.0), written from the published algorithms, not AMD's or Hyllian's files.

## A release anyone can install (v61)

* **Icon:** title/sce_sys/icon0.png (drawn by this project: discs on a wheel and the name), put in the title by
  make_cdi_title.py.
* **INSTALL.txt** (title/INSTALL.txt): what a user needs, where the folder goes, the BIOS, games, NAS, the
  controls, the logs, the licences.
* **scripts/make_release.py <title folder> <version> [--with-core-source]**, after ps5/tools/build.sh:
  dist/release-cdi-<version>/ with CD-i-Wheel-<version>.zip (PPSA99142/ as built, never data/ or test files,
  plus INSTALL.txt), this project's source, with --with-core-source the SAME_CDI core and libnfs as built (their
  GPL / LGPL ask for it), NOTICES/ and SHA256SUMS.

Homebrew titles are installed as a folder in /data/homebrew/<TITLE_ID>/ (what the template builds and deploys),
not as a .pkg: making a console .pkg needs tools this project does not have. The template's backgrounds
(pic0.dds / pic1.dds, BC7) are optional and not made here.

## v62: why the picture filters are missing, and naming

* The touch pad menu now always lists Picture. When the GPU part of the filters could not be set up on the
  console, it says "not available", and title_log.txt says which step failed ("CDI: the picture filters could not
  start: ..."); when they work, "CDI: picture filters ready".
* "Smooth video" is now called "Frame blending (even motion)": it blends the newest emulator frame with the one
  before when the TV's refresh does not line up with the CD-i's 50 Hz, so motion is even. It does not change
  sharpness; the Picture setting does that.

## v63: filters on the real pixels, Resume, motion aiming

* **Filters on the real pixels.** A 384-pixel CD-i picture comes in a 768-wide frame, each pixel twice (and some
  frames have every line twice). The filters used to work on those doubled pixels, so Sharp, Smooth and Pixel art
  looked almost like Default. Each frame is now checked for doubled columns and rows (title: doubledPixels) and a
  first pass (PREPARE) takes one texel per real pixel before the filter runs. title_log.txt says what each filter
  got: "CDI: picture Sharp: filtered (1440x1080 from 768x207, pixels two columns wide)".
* **Resume no longer presses the game's button.** The button that closes the menu is kept from the game until it
  is let go (the title holds it back; the engine's own hold could miss it).
* **Motion aiming with Pointer: absolute.** The absolute mode put the pointer where the sticks pointed every frame,
  so moves from motion aiming, the touch pad or a mouse were undone at once. They now move the point the sticks aim
  around (the centre until then), in both modes.
* The touch pad / motion / mouse layer writes what it finds to title_log.txt ("CDI: pad extras", "CDI: pad 0:
  motion seen (gyro ...)", "CDI: USB mouse").
