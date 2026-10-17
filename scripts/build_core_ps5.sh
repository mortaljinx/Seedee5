#!/usr/bin/env bash
# Phase 3: cross-compile the SAME_CDI core with the PS5 payload SDK compiler.
# Builds static archives (lib*.a) only; the final .so link at the end of the
# libretro Makefile is expected to fail on the PS5 toolchain and can be ignored.
# Needs: PS5_PAYLOAD_SDK set, libc++ installed in the SDK (libcxx.sh).
set -uo pipefail
cd "$(dirname "$0")/.."
: "${PS5_PAYLOAD_SDK:?set PS5_PAYLOAD_SDK first}"

mkdir -p third_party
[ -d third_party/same_cdi_ps5 ] || git clone https://github.com/libretro/same_cdi third_party/same_cdi_ps5
./scripts/apply_core_patches.sh || echo "(continuing without the NVRAM patch)"
cd third_party/same_cdi_ps5

make -f Makefile.libretro -k -j"$(nproc)" platform=unix \
    CC="$PS5_PAYLOAD_SDK/bin/prospero-clang" \
    CXX="$PS5_PAYLOAD_SDK/bin/prospero-clang++" \
    AR="$PS5_PAYLOAD_SDK/bin/prospero-ar" > ../../build_core_ps5.log 2>&1
echo "make finished (exit $?), full log: build_core_ps5.log"

echo "--- compile errors (first 30) ---"
grep -E "error:" ../../build_core_ps5.log | head -30 | cut -c1-240
echo "--- results ---"
echo "object files: $(find obj -name '*.o' | wc -l)"
ls -l obj/unix/*.a 2>/dev/null || echo "no archives built yet"
