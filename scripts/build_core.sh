#!/usr/bin/env bash
# Builds the SAME_CDI libretro core on Ubuntu and copies it next to the host.
# Expect a long first build (MAME-derived) and several GB of free RAM/disk.
set -euo pipefail
cd "$(dirname "$0")/.."

sudo apt-get update
sudo apt-get install -y build-essential git curl python3 libsdl2-dev zlib1g-dev

mkdir -p third_party
if [ ! -d third_party/same_cdi ]; then
    git clone https://github.com/libretro/same_cdi third_party/same_cdi
fi
./scripts/apply_core_patches.sh || echo "(continuing without the NVRAM patch)"
cd third_party/same_cdi
make -f Makefile.libretro -j"$(nproc)"

so=$(find . -maxdepth 2 -name '*_libretro.so' | head -n1)
[ -n "$so" ] || { echo "build finished but no *_libretro.so found"; exit 1; }
cp "$so" ../../same_cdi_libretro.so
echo "core ready: same_cdi_libretro.so"
