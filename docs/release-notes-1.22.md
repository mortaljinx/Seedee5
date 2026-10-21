## Seedee5 1.22

### Fixed: updating with ProsperoStore wiped your data

ProsperoStore updates an app by replacing its whole folder, and everything Seedee5 kept (games, BIOS, covers, saves, settings) was inside it. **Nothing was deleted:** the store moves the old folder to `/data/prosperostore/previous/PPSA99142/`, where it stays until that app's next update.

- **From 1.22,** Seedee5 also keeps your settings, NAS settings, saved games and BIOS in the title's own storage, which an update doesn't touch, and puts them back by itself after an update.
- **Games on the console, covers and save states** are too big for that storage. After an update, the wheel tells you to copy them back from the previous folder over FTP. Games on a NAS need nothing, and covers download again.

### Also fixed

- **NAS on another subnet:** *Find NAS on my network* only searched the console's own 10.0.3.x (or 192.168.1.x...). It now asks the whole network too, so a NAS at 10.0.0.8 next to a console at 10.0.3.21 is found. A NAS on a separate, routed network can still be entered by hand (*Settings › NAS › Enter the address*), which now starts from your own network's address.
- **Entering a NAS address while it was still searching** did nothing: the request was refused and the old search's progress stayed on screen. A new request now stops the search and takes over.
- **Type the share path:** a NAS that doesn't list its shares (NFS 4 only, or listing turned off) can be opened by typing the share's path on the Shares page, such as `/volume1/games`.
- Downloads look up names through your own network's router, not always 192.168.1.1.
- A rare crash in the NAS save backup message.

### Updating to 1.22 with ProsperoStore from 1.21 or older

Your current data moves to the previous folder once more, because older versions kept no copy. After updating:

1. Open an FTP client and go to `/data/prosperostore/previous/PPSA99142/data/`.
2. Copy everything in it to `/data/homebrew/PPSA99142/data/`.
3. Start Seedee5. From then on, updates keep your settings and saves.

**If your data was wiped by an earlier update,** copy it back from that same folder **before** updating to 1.22: the store keeps only the folder from the last update, and the next update replaces it.

### Install or update by hand

1. Download `Seedee5-1.22.zip` below and unzip it. It holds one folder, `PPSA99142`.
2. Copy `PPSA99142` over FTP to `/data/homebrew/`, over the old folder. Don't delete the old one first: your data is inside it.
3. On a new install, put the CD-i BIOS (`cdimono1.zip`, not included) in `PPSA99142/data/system/same_cdi/bios/`, and your games in `PPSA99142/data/roms/` or on a NAS.

No BIOS, games or keys are included. Check the downloads against `SHA256SUMS`.
