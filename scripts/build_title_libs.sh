#!/usr/bin/env bash
# Builds what Seedee5 (the native PS5 title, title/make_cdi_title.py) links besides the engine's own sources,
# with the title's own payload SDK (the pin in its ps5/tools/setup-sdk.sh), into build/title-libs/:
#
#   libsamecdi.a    the SAME_CDI core (third_party/same_cdi_title, with patches/0001 to 0004), all in one archive
#   libbearssl.a    BearSSL (MIT), behind host/tlsget.c (cover downloads)
#   liblzma7z.a     the LZMA SDK's 7z reader (public domain), behind host/arc7z.c
#   libnfs.a        libnfs (LGPL-2.1+) at the pinned commit with our patch, and its headers in include/ (the NAS)
#
#   scripts/build_title_libs.sh ~/ps5/Seedee5       for the console
#   scripts/build_title_libs.sh --host                               for the PC (the title's host reference build)
#
# The payload build (platform/ps5) keeps its own libraries: the title's SDK is a different one (the platform
# layer's fork, emulated thread-local storage), and objects from one must not go into the other.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
cd "$(dirname "$0")/.."
here=$PWD
out=$here/build/title-libs
jobs=$(nproc 2>/dev/null || echo 4)

if [[ ${1:-} == --host ]]; then
    mode=host
    CC=${CC:-cc}
    CXX=${CXX:-c++}
    AR=${AR:-ar}
    core_tree=third_party/same_cdi
    out=$here/build/title-libs-host
    extra=""
    toolchain=""
elif [[ -n ${1:-} && -f ${1}/ps5/tools/setup-sdk.sh ]]; then
    mode=ps5
    title=$(cd "$1" && pwd)
    sdk=$(bash "$title/ps5/tools/setup-sdk.sh")
    [[ -x $sdk/bin/prospero-clang && -x $sdk/bin/prospero-clang++ ]] || { echo "no compilers in $sdk/bin" >&2; exit 2; }
    export PS5_CLANG=${PS5_CLANG:-$(command -v clang || true)}
    export PS5_PAYLOAD_SDK=$sdk
    CC=$sdk/bin/prospero-clang
    CXX=$sdk/bin/prospero-clang++
    AR=$sdk/bin/prospero-ar
    core_tree=third_party/same_cdi_title
    # what the template's compiler wrapper adds, said again (harmless if the SDK's wrappers add it too), and the
    # frame pointers the console's crash backtraces need
    extra="-femulated-tls -fno-plt -fno-stack-protector -fno-omit-frame-pointer -ffunction-sections -fdata-sections"
    toolchain=$sdk/toolchain/prospero.cmake
    echo "title SDK: $sdk"
else
    echo "usage: $0 <the Seedee5 title folder> | --host" >&2
    exit 2
fi
mkdir -p "$out" build/title-objs

# ---- BearSSL and the LZMA SDK: a few dozen C files each ----
build_c_lib() { # name source-dir flags...
    local name=$1 dir=$2
    shift 2
    local objs=() o
    mkdir -p "build/title-objs/$name"
    for f in "$dir"/*.c "$dir"/*/*.c; do
        [[ -f $f ]] || continue
        o="build/title-objs/$name/$(echo "${f#$dir/}" | tr / _).o"
        if [[ ! -f $o || $f -nt $o ]]; then
            $CC -O2 -w $extra "$@" -c "$f" -o "$o"
        fi
        objs+=("$o")
    done
    rm -f "$out/lib$name.a"
    $AR rcs "$out/lib$name.a" "${objs[@]}"
    echo "==> $out/lib$name.a (${#objs[@]} objects)"
}
build_c_lib bearssl host/third_party/bearssl/src -Ihost/third_party/bearssl/inc -Ihost/third_party/bearssl/src
build_c_lib lzma7z host/third_party/lzma -DZ7_PPMD_SUPPORT -DZ7_ST -Ihost/third_party/lzma

# ---- libnfs ----
if [[ $mode == ps5 ]]; then
    nfs_src=third_party/libnfs
    [[ -d $nfs_src/.git ]] || git clone https://github.com/sahlberg/libnfs "$nfs_src"
    git -C "$nfs_src" checkout -q 57adf64
    patch=$here/patches/libnfs-0001-reserved-port.patch
    git -C "$nfs_src" apply --check "$patch" 2>/dev/null && git -C "$nfs_src" apply "$patch"
    cmake -S "$nfs_src" -B build/title-libnfs \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$out" \
        -DCMAKE_C_FLAGS="$extra" -DBUILD_SHARED_LIBS=OFF -DENABLE_TESTS=OFF -DENABLE_DOCUMENTATION=OFF \
        -DENABLE_EXAMPLES=OFF -DENABLE_UTILS=OFF -DENABLE_MULTITHREADING=OFF > build/title-libnfs.log 2>&1 \
        || { tail -30 build/title-libnfs.log; exit 1; }
    cmake --build build/title-libnfs -j"$jobs" >> build/title-libnfs.log 2>&1 || { tail -30 build/title-libnfs.log; exit 1; }
    cmake --install build/title-libnfs >> build/title-libnfs.log 2>&1
    [[ -f $out/lib/libnfs.a ]] && mv -f "$out/lib/libnfs.a" "$out/libnfs.a"
    echo "==> $out/libnfs.a"
elif [[ -n ${NFS_DIR:-} && -f $NFS_DIR/lib/libnfs.a ]]; then
    cp -f "$NFS_DIR/lib/libnfs.a" "$out/libnfs.a"
    mkdir -p "$out/include" && cp -a "$NFS_DIR/include/." "$out/include/"
    echo "==> $out/libnfs.a (from $NFS_DIR)"
fi

# ---- the SAME_CDI core ----
if [[ ! -d $core_tree/.git ]]; then
    git clone https://github.com/libretro/same_cdi "$core_tree"
fi
for p in patches/0[0-9][0-9][0-9]-*.patch; do
    if git -C "$core_tree" apply --check "$here/$p" 2>/dev/null; then
        git -C "$core_tree" apply "$here/$p" && echo "applied $p"
    fi
done
if [[ $mode == ps5 ]]; then
    # the archives only: the libretro makefile's final .so link fails with these compilers, which is expected
    (cd "$core_tree" && make -f Makefile.libretro -k -j"$jobs" platform=unix CC="$CC $extra" CXX="$CXX $extra" AR="$AR" \
        > "$here/build/title-core.log" 2>&1) || true
else
    (cd "$core_tree" && make -f Makefile.libretro -j"$jobs" > "$here/build/title-core.log" 2>&1) || true
fi
obj=$core_tree/obj/unix
members=("$obj/gen/mame/mess/drivlist.o" "$obj/gen/version.o" "$obj/src/mame/mess.o"
    "$obj/src/osd/libretro/libretro-internal/libretro.o" "$obj/src/osd/libretro/retromain.o")
archives=(libmame_mess libfrontend libemu liboptional libutils libosd_retro libocore_retro libutf8proc)
for f in "${members[@]}"; do
    [[ -s $f ]] || { echo "the core did not build: $f is missing (see build/title-core.log)" >&2; grep -m 20 "error:" build/title-core.log >&2 || true; exit 1; }
done
for a in "${archives[@]}"; do
    [[ -s $obj/$a.a ]] || { echo "the core did not build: $obj/$a.a is missing (see build/title-core.log)" >&2; grep -m 20 "error:" build/title-core.log >&2 || true; exit 1; }
done
# one archive: the five objects and every member of the eight archives
{
    echo "create $out/libsamecdi.a"
    for a in "${archives[@]}"; do echo "addlib $obj/$a.a"; done
    for f in "${members[@]}"; do echo "addmod $f"; done
    echo "save"
    echo "end"
} > build/title-objs/samecdi.mri
rm -f "$out/libsamecdi.a"
$AR -M < build/title-objs/samecdi.mri
echo "==> $out/libsamecdi.a ($(du -h "$out/libsamecdi.a" | cut -f1))"
echo "done: $out"
