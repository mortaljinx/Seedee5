## Seedee5 1.23

### Fixed: connecting to a NAS

- **Entering a NAS address while "Find NAS" was still searching did nothing.** The request was refused, no shares appeared, and the old search's progress stayed on screen. A new request now stops the search and asks the NAS straight away.
- **Type the share path:** a NAS that doesn't list its shares (NFS 4 only, or listing turned off) can now be opened by typing the share's path on the Shares page, such as `/volume1/games`, with the on-screen keyboard. Then pick the folder with your games as usual.

Seedee5 speaks NFS 3 and NFS 4 (4.0). A NAS set to NFS 4.1 normally accepts 4.0 too: choose NFS 4 in Settings › NAS › NFS version.

### Updating

- **With ProsperoStore from 1.22:** your settings, NAS settings, saved games and BIOS come back by themselves. Copy games you keep on the console (`data/roms`) back from `/data/prosperostore/previous/PPSA99142/data/` over FTP.
- **From 1.21 or older:** copy everything in `/data/prosperostore/previous/PPSA99142/data/` back to `/data/homebrew/PPSA99142/data/` over FTP. The wheel reminds you.
- **By hand:** copy the `PPSA99142` folder from `Seedee5-1.23.zip` over the old one on your PS5 (`/data/homebrew/`).

No BIOS, games or keys are included. Check the downloads against `SHA256SUMS`.
