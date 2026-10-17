#!/usr/bin/env python3
"""Fetch CD-i box art for your games from the libretro-thumbnails project.

It matches each game file name (Mad_Dog_McCree.chd, TheApprentice.cue, ...) to
the community-maintained "Philips - CD-i" box art set used by RetroArch, downloads
the matching pictures and stores them as <game file name>.png, which is exactly
what the cover launcher looks for.

  # games on this computer, covers written to ./covers
  python3 tools/fetch_covers.py --roms ~/Documents/roms --covers ./covers

  # games and covers on the console, over FTP (the FTP payload must be running)
  python3 tools/fetch_covers.py --ftp <console-ip>:2121

Only the standard library is needed. Nothing is bundled with the emulator: the
pictures are downloaded from the public repository at
https://github.com/libretro-thumbnails/Philips_-_CD-i and are meant for personal
use in your own game list.
"""
import argparse
import difflib
import ftplib
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request

REPO = "libretro-thumbnails/Philips_-_CD-i"
RAW = "https://raw.githubusercontent.com/%s/master/Named_Boxarts/" % REPO
ALT = "https://thumbnails.libretro.com/Philips%20-%20CD-i/Named_Boxarts/"
TREE = "https://api.github.com/repos/%s/git/trees/master?recursive=1" % REPO
GAME_EXTS = (".cue", ".chd", ".iso")
IMAGE_EXTS = (".png", ".jpg", ".jpeg", ".bmp")
BAD_TAGS = ("beta", "demo", "proto", "sample", "promo", "unl", "preview")


def http_get(url, timeout=40):
    req = urllib.request.Request(url, headers={"User-Agent": "ps5-cdi-fetch-covers/1.0"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


# --------------------------------------------------------------- the index

def index_from_github_api():
    data = json.loads(http_get(TREE).decode("utf-8"))
    if data.get("truncated"):
        raise RuntimeError("file list truncated")
    return [t["path"].split("/", 1)[1] for t in data["tree"]
            if t["path"].startswith("Named_Boxarts/") and t["path"].lower().endswith(".png")]


def index_from_git():
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["git", "clone", "-q", "--depth", "1", "--filter=blob:none",
                        "--no-checkout", "https://github.com/" + REPO, tmp + "/r"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        out = subprocess.run(["git", "-C", tmp + "/r", "ls-tree", "-r", "--name-only", "HEAD"],
                             check=True, capture_output=True, text=True).stdout
    return [p.split("/", 1)[1] for p in out.splitlines()
            if p.startswith("Named_Boxarts/") and p.lower().endswith(".png")]


def index_from_site():
    html = http_get(ALT).decode("utf-8", "replace")
    names = [urllib.parse.unquote(h) for h in re.findall(r'href="([^"?]+\.png)"', html)]
    if not names:
        raise RuntimeError("no files listed")
    return names


def load_index(cache, refresh):
    if not refresh and os.path.exists(cache):
        try:
            d = json.load(open(cache))
            if time.time() - d["time"] < 30 * 86400 and d["names"]:
                return d["names"]
        except (OSError, ValueError, KeyError):
            pass
    errors = []
    for label, fn in (("GitHub API", index_from_github_api), ("git", index_from_git),
                      ("thumbnails.libretro.com", index_from_site)):
        try:
            names = fn()
            if names:
                os.makedirs(os.path.dirname(os.path.abspath(cache)), exist_ok=True)
                json.dump({"time": time.time(), "names": names}, open(cache, "w"))
                return names
        except Exception as e:  # noqa: BLE001 - any failure just means "try the next source"
            errors.append("%s: %s" % (label, e))
    sys.exit("Could not get the list of box art files.\n  " + "\n  ".join(errors))


# ---------------------------------------------------------------- matching

def split_tags(name):
    """'Apprentice, The (Europe) (Rev 1)' -> ('Apprentice, The', ['europe', 'rev 1'])"""
    stem = os.path.splitext(name)[0]
    tags = [t.lower() for t in re.findall(r"[\(\[]([^\)\]]*)[\)\]]", stem)]
    title = re.split(r"\s*[\(\[]", stem, maxsplit=1)[0]
    return title.strip(), tags


def fix_article(title):
    """Redump style 'Apprentice, The' -> 'The Apprentice'."""
    m = re.match(r"^(.*),\s*(The|A|An|Le|La|Les|L'|Der|Die|Das|De|Het|Il|El)$", title.strip(), re.I)
    return (m.group(2) + " " + m.group(1)).replace("' ", "'") if m else title


def squish(text):
    text = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode()
    text = text.lower().replace("&", " and ")
    return re.sub(r"[^a-z0-9]+", "", text)


def title_keys(title):
    """Several spellings of one title that should count as equal."""
    t = fix_article(title.replace("_", " "))
    words = re.sub(r"[^a-z0-9' ]+", " ", unicodedata.normalize("NFKD", t).encode("ascii", "ignore").decode().lower()).split()
    keys = {squish(t)}
    keys.add(squish(" ".join(w for w in words if w != "the")))   # 'Link - The Faces of Evil' ~ 'Link Faces of Evil'
    for k in list(keys):                                          # 'TheApprentice' ~ 'Apprentice'
        if k.startswith("the") and len(k) > 6:
            keys.add(k[3:])
    keys.discard("")
    return keys


def disc_of(tags):
    for t in tags:
        m = re.search(r"disc\s*(\d+)", t)
        if m:
            return int(m.group(1))
    return None


def _digits(k):
    return "".join(ch for ch in k if ch.isdigit())


def title_score(rom_keys, cand_keys):
    """Same rules as the C version in host/main.c (keep them in step)."""
    if rom_keys & cand_keys:
        return 100.0
    best = 0.0
    for a in rom_keys:
        for b in cand_keys:
            la, lb = len(a), len(b)
            if not la or not lb:
                continue
            rom_is_prefix = la >= 6 and lb > la and b.startswith(a) and la * 100 // lb >= 35
            if la > lb and lb >= 3 and a.startswith(b):
                continue   # 'Crime Patrol 2' is not the cover of 'Crime Patrol'
            if not rom_is_prefix and 200.0 * min(la, lb) / (la + lb) < 60.0:
                continue
            if _digits(a) != _digits(b):
                continue   # different numbers: a different game
            r = 82.0 if rom_is_prefix else lcs_ratio(a, b)
            best = max(best, r)
    return best


def lcs_ratio(a, b):
    """200 * LCS / (len a + len b): what the C version computes."""
    prev = [0] * (len(b) + 1)
    for x in a:
        cur = [0]
        for j, y in enumerate(b, 1):
            cur.append(prev[j - 1] + 1 if x == y else max(prev[j], cur[j - 1]))
        prev = cur
    return 200.0 * prev[-1] / (len(a) + len(b))


def best_matches(rom_name, names, prefer, limit=4):
    title, tags = split_tags(rom_name)
    rkeys = title_keys(title)
    rdisc = disc_of(tags)
    rregions = [r for r in prefer if any(r.lower() in t for t in tags)]
    scored = []
    for n in names:
        ctitle, ctags = split_tags(n)
        s = title_score(rkeys, title_keys(ctitle))
        if s < 60:
            continue
        # tie-breakers, small enough never to beat a better title match
        bonus = 0.0
        cdisc = disc_of(ctags)
        if rdisc is not None:
            bonus += 3 if cdisc == rdisc else -3
        elif cdisc not in (None, 1):
            bonus -= 3
        for i, region in enumerate(prefer):
            if any(region.lower() in t for t in ctags):
                bonus += 2.0 - 0.3 * i
                break
        if rregions and any(r.lower() in t for r in rregions for t in ctags):
            bonus += 4
        bonus -= 4 * sum(1 for t in ctags if any(b in t for b in BAD_TAGS))
        bonus -= 1 * sum(1 for t in ctags if t == "alt")
        scored.append((s + bonus / 10.0, s, n))
    scored.sort(key=lambda x: (-x[0], x[2]))
    return [(sc, raw, n) for sc, raw, n in scored[:limit]]


# --------------------------------------------------------------- file lists

def local_games(root):
    games = []

    def walk(d, depth):
        try:
            entries = sorted(os.listdir(d))
        except OSError:
            return
        for e in entries:
            if e.startswith("."):
                continue
            p = os.path.join(d, e)
            if os.path.isdir(p):
                if depth < 2:
                    walk(p, depth + 1)
            elif e.lower().endswith(GAME_EXTS):
                games.append(os.path.splitext(e)[0])
    walk(root, 0)
    return games


def ftp_list(ftp, path):
    """[(name, is_dir)] using MLSD when the server has it, LIST otherwise."""
    try:
        return [(n, f.get("type") == "dir") for n, f in ftp.mlsd(path, facts=["type"])
                if f.get("type") in ("dir", "file")]
    except (ftplib.error_perm, ftplib.error_reply, AttributeError):
        pass
    lines = []
    ftp.retrlines("LIST " + path, lines.append)
    out = []
    for line in lines:
        parts = line.split(None, 8)
        if len(parts) >= 9:
            out.append((parts[8], line.startswith("d")))
        elif len(parts) >= 2 and ";" in line:   # facts-style lines
            out.append((line.split(" ", 1)[-1], "type=dir" in line.lower()))
    return out


def ftp_games(ftp, root):
    games = []

    def walk(d, depth):
        try:
            entries = ftp_list(ftp, d)
        except ftplib.all_errors:
            return
        for name, is_dir in sorted(entries):
            if name.startswith(".") or name in (".", ".."):
                continue
            if is_dir:
                if depth < 2:
                    walk(d.rstrip("/") + "/" + name, depth + 1)
            elif name.lower().endswith(GAME_EXTS):
                games.append(os.path.splitext(name)[0])
    walk(root, 0)
    return games


def ftp_mkdirs(ftp, path):
    """Create the folder (and its parents) if it does not exist yet."""
    cur = ""
    for part in [p for p in path.split("/") if p]:
        cur += "/" + part
        try:
            ftp.cwd(cur)
        except ftplib.error_perm:
            ftp.mkd(cur)
    ftp.cwd("/")


def ftp_connect(spec, user, password):
    host, _, port = spec.partition(":")
    ftp = ftplib.FTP()
    ftp.connect(host, int(port) if port else 21, timeout=30)
    ftp.login(user, password)
    ftp.set_pasv(True)
    return ftp


# -------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description="Fetch CD-i box art from libretro-thumbnails.")
    ap.add_argument("--roms", help="folder with your games (searched two levels deep)")
    ap.add_argument("--covers", default="covers", help="where covers are written (default ./covers)")
    ap.add_argument("--ftp", metavar="HOST[:PORT]", help="work on the console over FTP instead of local folders")
    ap.add_argument("--ftp-user", default="anonymous")
    ap.add_argument("--ftp-password", default="")
    ap.add_argument("--ftp-roms", default="/data/ps5-cdi/roms")
    ap.add_argument("--ftp-covers", default="/data/ps5-cdi/covers")
    ap.add_argument("--prefer", default="Europe,USA,World",
                    help="region order when a game has several releases (default Europe,USA,World)")
    ap.add_argument("--min-score", type=float, default=80, help="lowest title score accepted (default 80)")
    ap.add_argument("--force", action="store_true", help="download again even if a cover exists")
    ap.add_argument("--dry-run", action="store_true", help="only show what would be fetched")
    ap.add_argument("--refresh", action="store_true", help="refresh the list of available box art")
    ap.add_argument("--cache", default=os.path.join(os.path.expanduser("~"), ".cache", "ps5-cdi", "cdi_boxarts.json"))
    ap.add_argument("--name", action="append", default=[], help="a game name to look up (can be repeated)")
    args = ap.parse_args()

    ftp = None
    if args.ftp:
        ftp = ftp_connect(args.ftp, args.ftp_user, args.ftp_password)
        games = ftp_games(ftp, args.ftp_roms)
        have = set()
        try:
            have = {os.path.splitext(n)[0] for n, d in ftp_list(ftp, args.ftp_covers)
                    if not d and n.lower().endswith(IMAGE_EXTS)}
        except ftplib.all_errors:
            pass
        where = "console %s:%s" % (args.ftp, args.ftp_roms)
    elif args.roms:
        games = local_games(args.roms)
        have = set()
        if os.path.isdir(args.covers):
            have = {os.path.splitext(n)[0] for n in os.listdir(args.covers) if n.lower().endswith(IMAGE_EXTS)}
        where = args.roms
    else:
        games, have, where = [], set(), "--name"
    games += args.name
    games = sorted(set(games), key=str.lower)
    if not games:
        sys.exit("No games found. Give --roms DIR, --ftp HOST:PORT or --name 'Game title'.")

    names = load_index(args.cache, args.refresh)
    prefer = [p.strip() for p in args.prefer.split(",") if p.strip()]
    print("%d game(s) in %s, %d box art pictures available\n" % (len(games), where, len(names)))
    if not args.dry_run and not ftp:
        os.makedirs(args.covers, exist_ok=True)
    if not args.dry_run and ftp:
        ftp_mkdirs(ftp, args.ftp_covers)

    got = skipped = missing = failed = 0
    for g in games:
        if g in have and not args.force:
            print("  have   %s" % g)
            skipped += 1
            continue
        found = best_matches(g, names, prefer)
        if not found or found[0][1] < args.min_score:
            print("  --     %s   (no match)" % g)
            for sc, raw, n in found[:3]:
                print("           closest: %s  [%d]" % (n, raw))
            missing += 1
            continue
        sc, raw, n = found[0]
        flag = "ok" if raw >= 95 else "??"
        print("  %-6s %s  <-  %s" % (flag, g, n))
        if args.dry_run:
            continue
        try:
            try:
                data = http_get(RAW + urllib.parse.quote(n))
            except urllib.error.URLError:
                data = http_get(ALT + urllib.parse.quote(n))
            if not data.startswith(b"\x89PNG"):
                raise ValueError("not a PNG")
            target = g + ".png"
            if ftp:
                ftp.storbinary("STOR " + args.ftp_covers.rstrip("/") + "/" + target, io.BytesIO(data))
            else:
                with open(os.path.join(args.covers, target), "wb") as f:
                    f.write(data)
            got += 1
        except Exception as e:  # noqa: BLE001
            print("           download failed: %s" % e)
            failed += 1

    print("\n%d downloaded, %d already there, %d without a match, %d failed" % (got, skipped, missing, failed))
    if got and not ftp and not args.dry_run:
        print("Copy the files in %s to /data/ps5-cdi/covers/ on the console." % args.covers)
    if ftp:
        ftp.quit()
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
