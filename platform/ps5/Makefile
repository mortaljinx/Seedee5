# PS5 build of cdihost. Needs: ps5-payload-sdk and the PS5 SDL2 port installed
# (see README). Phase 2 target: the dummy test core linked statically.
#
#   export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
#   make -C platform/ps5                      # build
#   PS5_HOST=<console ip> make -C platform/ps5 test   # send to your ELF loader

PS5_HOST ?= ps5
PS5_PORT ?= 9021
SDL      ?= $(HOME)/ps5-sdl

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

.DEFAULT_GOAL := all

ELF  := cdihost-test.elf
SRCS := ../../host/main.c ../../host/tlsget.c ../../host/arc7z.c ../../tools/testcore.c

# BearSSL (MIT), the TLS code behind host/tlsget.c: built once into build/libbearssl.a
BEAR_DIR := ../../host/third_party/bearssl
BEAR_SRC := $(wildcard $(BEAR_DIR)/src/*.c $(BEAR_DIR)/src/*/*.c)
BEAR_OBJ := $(patsubst $(BEAR_DIR)/src/%.c,build/bearssl/%.o,$(BEAR_SRC))
BEAR_CFLAGS := -O2 -w -I$(BEAR_DIR)/inc -I$(BEAR_DIR)/src

build/bearssl/%.o: $(BEAR_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BEAR_CFLAGS) -c -o $@ $<

build/libbearssl.a: $(BEAR_OBJ)
	$(AR) rcs $@ $^

# The 7z reader and LZMA decoders of the LZMA SDK (public domain), behind host/arc7z.c
LZMA_DIR := ../../host/third_party/lzma
LZMA_SRC := $(wildcard $(LZMA_DIR)/*.c)
LZMA_OBJ := $(patsubst $(LZMA_DIR)/%.c,build/lzma/%.o,$(LZMA_SRC))
LZMA_FLAGS := -DZ7_PPMD_SUPPORT -DZ7_ST -I$(LZMA_DIR)

build/lzma/%.o: $(LZMA_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -w $(LZMA_FLAGS) -c -o $@ $<

build/liblzma7z.a: $(LZMA_OBJ)
	$(AR) rcs $@ $^

# libnfs (LGPL-2.1+), the NFS client behind games on a NAS: build/libnfs-ps5 is made by
# scripts/build_libnfs_ps5.sh. Without it the program is built without NAS support.
NFS_DIR := ../../build/libnfs-ps5
ifneq ($(wildcard $(NFS_DIR)/lib/libnfs.a),)
NFS_CFLAGS := -DHAVE_NFS -I$(NFS_DIR)/include
NFS_LIB    := $(NFS_DIR)/lib/libnfs.a
NFS_SRC    := ../../host/nfsclient.c
$(info NAS support: libnfs found in $(NFS_DIR))
else
$(info NAS support: off (run scripts/build_libnfs_ps5.sh to add it))
endif

CFLAGS  := -O2 -g -Wall -Wextra -DPS5 -DSTATIC_CORE -DAUTO_EXIT_FRAMES=1800 -D_REENTRANT \
           -I../../include -I$(SDL)/include -I$(SDL)/include/SDL2 -I$(BEAR_DIR)/inc $(NFS_CFLAGS) $(LZMA_FLAGS)
# Our SDL is named by its full path, not with -L/-l: the SDK's compiler wrapper puts the SDK's own
# target/user/homebrew/lib in front of every -L, so a libSDL2.a installed there (another SDL build)
# would silently win.
LDFLAGS := $(SDL)/lib/libSDL2.a \
           -lSceSystemService -lSceUserService -lScePad -lSceAudioOut \
           -lSceVideoOut -lSceKeyboard -lSceImeDialog \
           -lSceNet -lSceNetCtl -lSceSsl -lSceHttp2 -lSceRandom -lpthread

all: $(ELF)

$(ELF): $(SRCS) $(NFS_SRC) build/libbearssl.a build/liblzma7z.a
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(NFS_SRC) build/liblzma7z.a build/libbearssl.a $(NFS_LIB) $(LDFLAGS)

clean:
	rm -f $(ELF) cdihost.elf host-cdi.o host-tls.o host-arc.o host-nfs.o

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^


# ---- Phase 3: real CD-i core ------------------------------------------------
# After scripts/build_core_ps5.sh has produced the archives:
#   make -C platform/ps5 cdihost.elf
CORE      ?= ../../third_party/same_cdi_ps5
COREOBJ   := $(CORE)/obj/unix
CORE_OBJS := $(COREOBJ)/gen/mame/mess/drivlist.o $(COREOBJ)/gen/version.o \
             $(COREOBJ)/src/mame/mess.o \
             $(COREOBJ)/src/osd/libretro/libretro-internal/libretro.o \
             $(COREOBJ)/src/osd/libretro/retromain.o
CORE_LIBS := -Wl,--start-group \
             $(addprefix $(COREOBJ)/,libmame_mess.a libfrontend.a libemu.a \
               liboptional.a libutils.a libosd_retro.a libocore_retro.a \
               libutf8proc.a) \
             -Wl,--end-group

HOSTCFLAGS := -O2 -g -Wall -Wextra -DPS5 -DSTATIC_CORE -D_REENTRANT \
              -I../../include -I$(SDL)/include -I$(SDL)/include/SDL2 -I$(BEAR_DIR)/inc $(NFS_CFLAGS) $(LZMA_FLAGS)

host-cdi.o: ../../host/main.c
	$(CC) $(HOSTCFLAGS) -c -o $@ $<

host-tls.o: ../../host/tlsget.c
	$(CC) $(HOSTCFLAGS) -c -o $@ $<

host-arc.o: ../../host/arc7z.c
	$(CC) $(HOSTCFLAGS) -c -o $@ $<

ifneq ($(NFS_SRC),)
host-nfs.o: ../../host/nfsclient.c
	$(CC) $(HOSTCFLAGS) -c -o $@ $<
NFS_OBJ := host-nfs.o
endif

cdihost.elf: host-cdi.o host-tls.o host-arc.o $(NFS_OBJ) build/libbearssl.a build/liblzma7z.a $(CORE_OBJS)
	$(CXX) -o $@ host-cdi.o host-tls.o host-arc.o $(NFS_OBJ) build/liblzma7z.a build/libbearssl.a $(NFS_LIB) $(CORE_OBJS) $(CORE_LIBS) $(LDFLAGS)

.PHONY: all clean test
