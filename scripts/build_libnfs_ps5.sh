#!/bin/bash
# Builds libnfs (NFS client library, LGPL-2.1+) for the console with the payload SDK and installs it
# into build/libnfs-ps5. The Makefiles pick it up from there; without it the program is built
# without NAS support. Needs git and cmake.
set -e
cd "$(dirname "$0")/.."
: "${PS5_PAYLOAD_SDK:=/opt/ps5-payload-sdk}"
SRC=third_party/libnfs
OUT="$PWD/build/libnfs-ps5"
COMMIT=57adf64   # the version this was tested with

mkdir -p third_party
[ -d "$SRC/.git" ] || git clone https://github.com/sahlberg/libnfs "$SRC"
git -C "$SRC" checkout -q "$COMMIT"
# our patch: connect from a reserved source port (many NAS boxes insist on it unless exported "insecure")
PATCH="$PWD/patches/libnfs-0001-reserved-port.patch"
if git -C "$SRC" apply --check "$PATCH" 2>/dev/null; then
    git -C "$SRC" apply "$PATCH" && echo "applied $(basename "$PATCH")"
else
    echo "$(basename "$PATCH") is already applied (or does not fit)"
fi

cmake -S "$SRC" -B "$OUT-build" \
    -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$OUT" \
    -DBUILD_SHARED_LIBS=OFF -DENABLE_TESTS=OFF -DENABLE_DOCUMENTATION=OFF \
    -DENABLE_EXAMPLES=OFF -DENABLE_UTILS=OFF -DENABLE_MULTITHREADING=OFF
cmake --build "$OUT-build" -j4
cmake --install "$OUT-build"
ls -l "$OUT/lib/libnfs.a"
