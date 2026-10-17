#!/usr/bin/env python3
"""Seedee5: a release that anyone can install, for GitHub and the homebrew stores.

    python3 scripts/make_release.py /mnt/data/ps5stack/PS5_CDiWheel 1.0 [--with-core-source]

Run from this project's folder after ps5/tools/build.sh. Writes dist/release-seedee5-<version>/ in the title's folder:

  Seedee5-<version>.zip             PPSA99142/ (the built title folder, as ps5/tools/build.sh made it) and
                                   INSTALL.txt; nothing the console wrote (data/, test files, screenshots, logs)
  Seedee5-<version>-source.tar.gz   this project's source (engine, host, title, scripts, patches, tools, docs)
  same_cdi-<rev>-source.tar.gz,    with --with-core-source: the core and libnfs at the revisions built, the
  libnfs-<rev>-source.tar.gz       patches applied (the GPL and LGPL ask for the corresponding source)
  NOTICES/                         the licences of the parts this project adds to the template's
  store/                           for a homebrew store: icon0.png (512x512), listing.json, description.txt
  SHA256SUMS

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

CDI = Path(__file__).resolve().parent.parent
NEVER_TOP = {"data", "test-run.txt", "test-results.txt", "screenshots", "klog", ".env", "radv-shader-cache", "hui"}
SOURCE_PARTS = ["Makefile", "README.md", "LICENSE", "CHANGELOG.md", ".gitignore", "docs", "engine", "host", "include", "patches",
                "platform", "scripts", "title", "tools"]
NOTICES = {
    "Seedee5-LICENSE.txt": ["LICENSE"],
    "same_cdi-COPYING.txt": ["third_party/same_cdi_title/COPYING", "third_party/same_cdi/COPYING"],
    "libnfs-COPYING.txt": ["third_party/libnfs/COPYING"],
    "libnfs-LICENCE-LGPL-2.1.txt": ["third_party/libnfs/LICENCE-LGPL-2.1.txt"],
    "bearssl-LICENSE.txt": ["host/third_party/bearssl/LICENSE.txt"],
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, check=True).stdout.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("title", type=Path)
    ap.add_argument("version")
    ap.add_argument("--with-core-source", action="store_true")
    a = ap.parse_args()
    title = a.title.resolve()
    tid = json.loads((title / "ps5/sce_sys/param.json").read_text())["titleId"]
    app = title / "dist" / tid
    if not (app / "eboot.bin").is_file():
        sys.exit(f"{app}/eboot.bin is missing: run ps5/tools/build.sh first")
    out = title / "dist" / f"release-seedee5-{a.version}"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    # the title, as the console runs it, and how to install it
    zpath = out / f"Seedee5-{a.version}.zip"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for p in sorted(app.rglob("*")):
            rel = p.relative_to(app)
            if rel.parts[0] in NEVER_TOP:
                continue
            arc = f"{tid}/{rel.as_posix()}"
            if p.is_dir():
                z.write(p, arc + "/")
            else:
                z.write(p, arc)
        z.write(CDI / "title/INSTALL.txt", "INSTALL.txt")
    with zipfile.ZipFile(zpath) as z:
        bad = z.testzip()
        if bad:
            sys.exit(f"the zip is damaged at {bad}")
        names = z.namelist()
    print(f"==> {zpath.name}: {len(names)} entries")

    # this project's source
    src = out / f"Seedee5-{a.version}-source.tar.gz"
    with tarfile.open(src, "w:gz") as t:
        for part in SOURCE_PARTS:
            p = CDI / part
            if p.exists():
                t.add(p, arcname=f"Seedee5-{a.version}/{part}",
                      filter=lambda ti: None if "__pycache__" in ti.name or ti.name.endswith(".o") else ti)
    print(f"==> {src.name}")

    # the core and libnfs as built (their own licences ask for it)
    if a.with_core_source:
        for name, tree in (("same_cdi", CDI / "third_party/same_cdi_title"), ("libnfs", CDI / "third_party/libnfs")):
            if not (tree / ".git").exists():
                print(f"   (no {tree}: skipped)")
                continue
            rev = git(tree, "rev-parse", "--short=12", "HEAD")
            dest = out / f"{name}-{rev}-source.tar.gz"
            # the commit, then the changes the build applied on top of it
            with open(dest, "wb") as f:
                subprocess.run(["git", "-C", str(tree), "archive", "--format=tar.gz", f"--prefix={name}-{rev}/", "HEAD"],
                               stdout=f, check=True)
            diff = git(tree, "diff", "HEAD")
            if diff:
                (out / f"{name}-{rev}-changes.patch").write_text(diff + "\n")
            print(f"==> {dest.name}")

    notices = out / "NOTICES"
    notices.mkdir()
    for dest, candidates in NOTICES.items():
        for c in candidates:
            if (CDI / c).is_file():
                shutil.copyfile(CDI / c, notices / dest)
                break
        else:
            print(f"   (no licence text for {dest}: add it before publishing)")
    (notices / "README.txt").write_text(
        "Seedee5 (GPL-3.0-or-later) contains: the SAME_CDI core (MAME; GPL-2.0-or-later and BSD-3-Clause),\n"
        "libnfs (LGPL-2.1-or-later), BearSSL (MIT), the LZMA SDK (public domain), stb_image (public domain / MIT).\n"
        "The PS5 Vulkan Template, its UI kit and runtime are MIT: their notices are in the title folder\n"
        "(LEGAL.txt, licenses/). Game facts (games_info.hpp) are from Wikipedia's List of CD-i games.\n"
        "A game card's text is fetched from Wikipedia at run time (CC BY-SA 4.0; the card names the article);\n"
        "covers, logos and screenshots are fetched from libretro-thumbnails. Neither is included.\n"
        "No BIOS, game or key is included.\n")

    # for a homebrew store: the icon, a listing, a description (the fields most stores ask for)
    store = out / "store"
    store.mkdir()
    icon = app / "sce_sys/icon0.png"
    shutil.copyfile(icon if icon.is_file() else CDI / "title/sce_sys/icon0.png", store / "icon0.png")
    short = "Philips CD-i emulator: a cover-wheel library, NAS games, picture filters and the FMV games."
    long = ("Seedee5 plays Philips CD-i games on the PS5 with the SAME_CDI core (MAME's CD-i emulation). "
            "Your games on a cover wheel with box art, favourites and recently played; games from the console, a USB "
            "drive or a NAS (NFS); straight into the game with Continue; save states, rewind and NAS save backups; "
            "picture filters (Sharp, Smooth, Enhanced, FSR, Pixel art, Scanlines, CRT); the Digital Video (FMV) "
            "games such as Mad Dog McCree and Burn:Cycle. Bring your own CD-i BIOS (cdimono1.zip) and games: none "
            "are included.")
    listing = {
        "name": "Seedee5",
        "title_id": tid,
        "version": a.version,
        "category": "Emulator",
        "author": "mortaljinx",
        "licence": "GPL-3.0-or-later",
        "short_description": short,
        "description": long,
        "requirements": "A PS5 that runs homebrew titles from /data/homebrew; the CD-i BIOS (cdimono1.zip) and your own games.",
        "install": f"Copy the {tid} folder from the zip to /data/homebrew/ over FTP, then start Seedee5.",
        "file": zpath.name,
        "size": zpath.stat().st_size,
        "sha256": sha256(zpath),
        "icon": "icon0.png",
    }
    (store / "listing.json").write_text(json.dumps(listing, indent=2) + "\n")
    (store / "description.txt").write_text(f"Seedee5 {a.version}\n\n{short}\n\n{long}\n")
    print(f"==> store/ (icon0.png, listing.json, description.txt)")

    sums = []
    for p in sorted(out.rglob("*")):
        if p.is_file() and p.name != "SHA256SUMS":
            sums.append(f"{sha256(p)}  {p.relative_to(out).as_posix()}")
    (out / "SHA256SUMS").write_text("\n".join(sums) + "\n")
    print(f"==> {out}")


if __name__ == "__main__":
    main()
