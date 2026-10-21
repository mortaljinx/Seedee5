#!/usr/bin/env python3
"""ps5-cdi: make (or update) Seedee5, the native PS5 title, on the PS5 Vulkan Template.

    python3 title/make_cdi_title.py ~/ps5/Seedee5
    python3 title/make_cdi_title.py ~/ps5/Seedee5 --update   (after a new ps5-cdi version)

Run from the ps5-cdi folder. The first form makes the title with the template's own generator
(ps5/tools/new-title.py --ui carousel, title id PPSA99142, 60 Hz), then puts Seedee5's files in:

  examples/cdiwheel/cdiwheel.cpp       the program (replaces the UI starter)
  examples/cdiwheel/kit/screen.cpp     the Cover Wheel design, changed for the game list
  examples/cdiwheel/settings.hpp       the settings screen (BIOS, NAS, games, covers)
  ps5/src/cdi/cdi_library.hpp          what the program and the design share
  ps5/src/cdi/cdi_libc*.c              libc names a title defines itself (numeric getaddrinfo and the like)
  ps5/src/prelude.h                    + the engine's and the list's headers, before a sample's namespace
  ps5/CMakeLists.txt                   + the engine (this folder's engine/ and host/), and the libraries
                                         scripts/build_title_libs.sh made (the CD-i core, BearSSL, LZMA, libnfs)

--update puts the files in again and leaves everything else alone (the title's git history, its build).
The template must be beside the title (../PS5_VulkanTemplate), as the Jellyfin title has it.

SPDX-License-Identifier: GPL-3.0-or-later
"""

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

CDI = Path(__file__).resolve().parent.parent  # the ps5-cdi folder
TITLE_FILES = {
    "examples/cdiwheel/cdiwheel.cpp": "title/examples/cdiwheel/cdiwheel.cpp",
    "examples/cdiwheel/kit/screen.cpp": "title/examples/cdiwheel/kit/screen.cpp",
    "examples/cdiwheel/settings.hpp": "title/examples/cdiwheel/settings.hpp",
    "examples/cdiwheel/picture.hpp": "title/examples/cdiwheel/picture.hpp",
    "examples/cdiwheel/picture_spv.h": "title/examples/cdiwheel/picture_spv.h",
    "examples/cdiwheel/picture.comp": "title/examples/cdiwheel/picture.comp",
    "examples/cdiwheel/controls.hpp": "title/examples/cdiwheel/controls.hpp",
    "examples/cdiwheel/logo_png.h": "title/examples/cdiwheel/logo_png.h",
    "examples/cdiwheel/games_info.hpp": "title/examples/cdiwheel/games_info.hpp",
    "ps5/sce_sys/icon0.png": "title/sce_sys/icon0.png",
    "ps5/src/cdi/cdi_library.hpp": "title/ps5/src/cdi/cdi_library.hpp",
    "ps5/src/cdi/cdi_libc.c": "title/ps5/src/cdi/cdi_libc.c",
    "ps5/src/cdi/cdi_libc_stubs.c": "title/ps5/src/cdi/cdi_libc_stubs.c",
}

PRELUDE_MARK = "// CD-i Wheel: the engine and the game list"
PRELUDE_ADD = f"""
{PRELUDE_MARK} (ps5-cdi's title/make_cdi_title.py)
#include <cctype>
#include "cdie.h"
#include "cdi_library.hpp"
"""

CMAKE_MARK = "# ---- CD-i Wheel"
CMAKE_ADD = """
# ---- CD-i Wheel: the engine (ps5-cdi's engine/, shared with its SDL payload) and what it links ----
# Put here by ps5-cdi's title/make_cdi_title.py. CDI_DIR is the ps5-cdi folder; its build/title-libs holds the
# libraries scripts/build_title_libs.sh built with this title's SDK: the SAME_CDI core (libsamecdi.a), BearSSL,
# the LZMA SDK's 7z reader and, when it was built, libnfs (games on a NAS).
set(CDI_DIR "@CDI_DIR@" CACHE PATH "the ps5-cdi folder" FORCE) # the folder make_cdi_title.py last ran from
set(CDI_LIBS_DIR "${CDI_DIR}/build/title-libs" CACHE PATH "the libraries built for the title")
option(CDI_TESTCORE "host reference only: link ps5-cdi's test core (a moving picture) instead of the CD-i core" OFF)
set(cdi_sources ${CDI_DIR}/engine/cdi_engine.c ${CDI_DIR}/host/tlsget.c ${CDI_DIR}/host/arc7z.c ${CDI_DIR}/host/gameinfo.c)
set(cdi_defines STATIC_CORE Z7_PPMD_SUPPORT Z7_ST)
# the title id, for the FTP paths the screens show (/app0 is /data/homebrew/<id>/ over FTP)
target_compile_definitions(samples PRIVATE "CDI_TITLE_ID=\\"@TITLE_ID@\\"")
if(NOT PS5_HOST_REFERENCE)
	list(APPEND cdi_defines PS5)
	# the libc names the title defines itself (numeric getaddrinfo for libnfs, and what title-local.map names)
	list(APPEND cdi_sources ${CMAKE_CURRENT_SOURCE_DIR}/src/cdi/cdi_libc.c ${CMAKE_CURRENT_SOURCE_DIR}/src/cdi/cdi_libc_stubs.c)
endif()
if(EXISTS ${CDI_LIBS_DIR}/libnfs.a)
	list(APPEND cdi_sources ${CDI_DIR}/host/nfsclient.c)
	list(APPEND cdi_defines HAVE_NFS)
	message(STATUS "Seedee5: NAS support (libnfs in ${CDI_LIBS_DIR})")
else()
	message(STATUS "Seedee5: no libnfs.a in ${CDI_LIBS_DIR}: built without NAS support")
endif()
if(PS5_HOST_REFERENCE AND CDI_TESTCORE)
	list(APPEND cdi_sources ${CDI_DIR}/tools/testcore.c)
endif()
set_source_files_properties(${cdi_sources} PROPERTIES COMPILE_OPTIONS "-w" COMPILE_DEFINITIONS "${cdi_defines}")
target_sources(samples PRIVATE ${cdi_sources})
target_include_directories(samples PRIVATE
	src/cdi
	${CDI_DIR}/engine
	${CDI_DIR}/host
	${CDI_DIR}/include
	${CDI_DIR}/host/third_party/bearssl/inc
	${CDI_DIR}/host/third_party/lzma
	${CDI_LIBS_DIR}/include)
set(CDI_ARCHIVES ${CDI_LIBS_DIR}/libbearssl.a ${CDI_LIBS_DIR}/liblzma7z.a)
if(NOT (PS5_HOST_REFERENCE AND CDI_TESTCORE))
	list(APPEND CDI_ARCHIVES ${CDI_LIBS_DIR}/libsamecdi.a)
endif()
if(EXISTS ${CDI_LIBS_DIR}/libnfs.a AND NOT PS5_HOST_REFERENCE)
	# libnfs resolves the NAS's address with getaddrinfo, which the link recipe binds to the platform layer's
	# (--defsym getaddrinfo=ps5_getaddrinfo); that one failed a plain IPv4 address with EAI_FAIL on the console.
	# A copy of libnfs.a calls the title's own numeric version instead (src/cdi/cdi_libc.c).
	find_program(CDI_OBJCOPY NAMES llvm-objcopy llvm-objcopy-18 llvm-objcopy-17 llvm-objcopy-16 objcopy REQUIRED)
	set(cdi_nfs ${CMAKE_CURRENT_BINARY_DIR}/libnfs-cdi.a)
	add_custom_command(OUTPUT ${cdi_nfs}
		COMMAND ${CDI_OBJCOPY} --redefine-sym getaddrinfo=cdi_getaddrinfo --redefine-sym freeaddrinfo=cdi_freeaddrinfo
			${CDI_LIBS_DIR}/libnfs.a ${cdi_nfs}
		DEPENDS ${CDI_LIBS_DIR}/libnfs.a
		COMMENT "Seedee5: libnfs with the title's getaddrinfo")
	list(APPEND CDI_ARCHIVES ${cdi_nfs})
elseif(EXISTS ${CDI_LIBS_DIR}/libnfs.a)
	list(APPEND CDI_ARCHIVES ${CDI_LIBS_DIR}/libnfs.a)
endif()
foreach(archive IN LISTS CDI_ARCHIVES)
	if(NOT EXISTS ${archive} AND NOT archive STREQUAL "${cdi_nfs}")
		message(FATAL_ERROR "Seedee5: ${archive} is missing: run ps5-cdi's scripts/build_title_libs.sh first")
	endif()
endforeach()
"""


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    subprocess.run(cmd, check=True, **kw)


def patch_prelude(title):
    p = title / "ps5/src/prelude.h"
    text = p.read_text()
    if PRELUDE_MARK in text:
        return
    anchor = '#if defined(PS5_UI)\n#include "kit.hpp"\n#endif\n'
    if anchor not in text:
        sys.exit(f"{p}: no kit.hpp block to add the engine's headers after")
    p.write_text(text.replace(anchor, anchor + PRELUDE_ADD))


def patch_cmake(title):
    p = title / "ps5/CMakeLists.txt"
    text = p.read_text()
    import json
    tid = json.loads((title / "ps5/sce_sys/param.json").read_text()).get("titleId", "PPSA99142")
    add = CMAKE_ADD.replace("@CDI_DIR@", str(CDI)).replace("@TITLE_ID@", tid)
    if CMAKE_MARK in text:
        start = text.index(CMAKE_MARK) - 1
        end = text.index("endforeach()\n", start) + len("endforeach()\n")
        text = text[:start] + add + text[end:]
        p.write_text(text)
        return
    host = "if(PS5_HOST_REFERENCE)\n\tadd_executable(ps5-samples-host $<TARGET_OBJECTS:samples>)\n" \
           "\ttarget_link_libraries(ps5-samples-host PRIVATE dl pthread)\n"
    if host not in text:
        sys.exit(f"{p}: the host reference link is not where it was")
    text = text.replace(host, add + "\n" + host.replace("PRIVATE dl pthread", "PRIVATE ${CDI_ARCHIVES} dl pthread"))
    link = ('"$<TARGET_OBJECTS:samples>"\n\tDEPENDS samples "$<TARGET_OBJECTS:samples>" '
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/link-title.sh")
    if link not in text:
        sys.exit(f"{p}: the title's link command is not where it was")
    text = text.replace(link, '"$<TARGET_OBJECTS:samples>" ${CDI_ARCHIVES}\n\tDEPENDS samples "$<TARGET_OBJECTS:samples>" '
                        "${CDI_ARCHIVES} ${CMAKE_CURRENT_SOURCE_DIR}/tools/link-title.sh")
    p.write_text(text)


APP_NAME = "Seedee5"
APP_VERSION = "1.23"  # keep in step with SEEDEE5_VERSION in ps5/src/cdi/cdi_library.hpp


def set_param(title):
    """The name and version the PS5 home screen shows: every titleName in param.json (each language), and the
    version keys it has (contentVersion 01.000.000, masterVersion / appVer 01.00)."""
    p = title / "ps5/sce_sys/param.json"
    if not p.is_file():
        print(f"   (no {p}: name and version not set)")
        return
    data = json.loads(p.read_text())
    major, minor = APP_VERSION.split(".")
    versions = {"contentVersion": f"{int(major):02d}.{minor:0<3}.000", "masterVersion": f"{int(major):02d}.{minor}",
                "appVer": f"{int(major):02d}.{minor}"}
    changed = []

    def walk(node, where):
        if isinstance(node, dict):
            for k, v in node.items():
                if k == "titleName" and isinstance(v, str) and v != APP_NAME:
                    node[k] = APP_NAME
                    changed.append(f"{where}{k}")
                elif k in versions and isinstance(v, str) and v != versions[k]:
                    node[k] = versions[k]
                    changed.append(f"{where}{k}")
                else:
                    walk(v, f"{where}{k}.")
        elif isinstance(node, list):
            for i, v in enumerate(node):
                walk(v, f"{where}{i}.")

    walk(data, "")
    if changed:
        p.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n")
        print(f"   param.json: {', '.join(changed)} -> {APP_NAME} {APP_VERSION}")
    else:
        print(f"   param.json: already {APP_NAME} {APP_VERSION}")
    # a copy in this project (title/sce_sys/param.json), committed with each release: the homebrew store reads the
    # contentVersion from the repository to tell consoles there is an update. (Only a record: the title's own
    # param.json is the one built in.)
    shutil.copyfile(p, CDI / "title/sce_sys/param.json")


def put_files(title):
    for dest, src in TITLE_FILES.items():
        d = title / dest
        d.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(CDI / src, d)
        print("  ", dest)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("title", type=Path, help="the title's folder, beside PS5_VulkanTemplate")
    ap.add_argument("--update", action="store_true", help="the title exists: only put Seedee5's files in again")
    ap.add_argument("--title-id", default="PPSA99142")
    ap.add_argument("--template", type=Path, help="the PS5_VulkanTemplate folder (default: beside the title)")
    args = ap.parse_args()
    title = args.title.resolve()
    template = (args.template or title.parent / "PS5_VulkanTemplate").resolve()

    if not args.update:
        if title.exists() and any(title.iterdir()):
            sys.exit(f"{title} exists: use --update to refresh Seedee5's files in it")
        if not (template / "ps5/tools/new-title.py").is_file():
            sys.exit(f"no PS5_VulkanTemplate at {template} (give --template)")
        run([sys.executable, str(template / "ps5/tools/new-title.py"), str(title), "--title-id", args.title_id,
             "--name", APP_NAME, "--ui", "carousel", "--refresh", "60"], cwd=template)
    elif not (title / "ps5/CMakeLists.txt").is_file():
        sys.exit(f"{title} is not a title made by new-title.py")

    print("Seedee5's files:")
    put_files(title)
    set_param(title)
    patch_prelude(title)
    patch_cmake(title)
    libs = CDI / "build/title-libs"
    if not (libs / "libsamecdi.a").is_file():
        print(f"\nnote: {libs}/libsamecdi.a is not built yet: run scripts/build_title_libs.sh {title} first")
    print(f"\nnext: cd {title} && ps5/tools/build.sh && ps5/tools/deploy.sh --all")


if __name__ == "__main__":
    main()
