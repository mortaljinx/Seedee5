CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)
HDR_URL := https://raw.githubusercontent.com/libretro/libretro-common/master/include/libretro.h

all: cdihost testcore.so

include/libretro.h:
	mkdir -p include
	curl -fsSL $(HDR_URL) -o $@

BEAR_DIR := host/third_party/bearssl
BEAR_SRC := $(wildcard $(BEAR_DIR)/src/*.c $(BEAR_DIR)/src/*/*.c)
BEAR_OBJ := $(patsubst $(BEAR_DIR)/src/%.c,build/bearssl/%.o,$(BEAR_SRC))
BEAR_CFLAGS := -O2 -w -I$(BEAR_DIR)/inc -I$(BEAR_DIR)/src

# The 7z reader and LZMA decoders of the LZMA SDK (public domain), behind host/arc7z.c
LZMA_DIR := host/third_party/lzma
LZMA_SRC := $(wildcard $(LZMA_DIR)/*.c)
LZMA_OBJ := $(patsubst $(LZMA_DIR)/%.c,build/lzma/%.o,$(LZMA_SRC))
LZMA_FLAGS := -DZ7_PPMD_SUPPORT -DZ7_ST -I$(LZMA_DIR)

build/lzma/%.o: $(LZMA_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -w $(LZMA_FLAGS) -c -o $@ $<

build/liblzma7z.a: $(LZMA_OBJ)
	ar rcs $@ $^

# BearSSL (MIT): the TLS code behind host/tlsget.c, built once into build/libbearssl.a
build/bearssl/%.o: $(BEAR_DIR)/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BEAR_CFLAGS) -c -o $@ $<

build/libbearssl.a: $(BEAR_OBJ)
	ar rcs $@ $^

# NAS support (games over NFS) is optional: it needs libnfs (apt install libnfs-dev), or NFS_DIR=<prefix>
ifneq ($(NFS_DIR),)
NFS_CFLAGS := -DHAVE_NFS -I$(NFS_DIR)/include
NFS_LIBS   := $(NFS_DIR)/lib/libnfs.a -lpthread
else ifneq ($(shell pkg-config --exists libnfs 2>/dev/null && printf 'int main(void){return 0;}' | $(CC) -x c - $$(pkg-config --libs libnfs) -o /dev/null 2>/dev/null && echo yes),)
NFS_CFLAGS := -DHAVE_NFS $(shell pkg-config --cflags libnfs)
NFS_LIBS   := $(shell pkg-config --libs libnfs)
endif
ifneq ($(NFS_CFLAGS),)
NFS_SRC := host/nfsclient.c host/nfsclient.h
endif

cdihost: host/main.c host/tlsget.c host/tlsget.h host/arc7z.c host/arc7z.h $(NFS_SRC) host/stb_image.h host/font8x16.h host/cdi_boxarts.h include/libretro.h build/libbearssl.a build/liblzma7z.a
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(NFS_CFLAGS) $(LZMA_FLAGS) -Iinclude -I$(BEAR_DIR)/inc -o $@ host/main.c host/tlsget.c host/arc7z.c $(filter %.c,$(NFS_SRC)) build/liblzma7z.a build/libbearssl.a $(NFS_LIBS) $(SDL_LIBS) -ldl

# The engine (engine/cdie.h: everything but drawing, as the PS5 title uses it) and its desktop test program
ENGINE_SRC := engine/cdi_engine.c host/tlsget.c host/arc7z.c host/gameinfo.c $(filter %.c,$(NFS_SRC))
cdie_sdl: tools/cdie_sdl.c $(ENGINE_SRC) engine/cdie.h host/stb_image.h host/cdi_boxarts.h host/cdi_extras.h host/gameinfo.h include/libretro.h build/libbearssl.a build/liblzma7z.a
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(NFS_CFLAGS) $(LZMA_FLAGS) -Iengine -Ihost -Iinclude -I$(BEAR_DIR)/inc -o $@ tools/cdie_sdl.c $(ENGINE_SRC) build/liblzma7z.a build/libbearssl.a $(NFS_LIBS) $(SDL_LIBS) -ldl -lpthread

# A game card's extras, end to end (tools/extras_test.c: the engine with one game; Wikipedia via tools/fake_wikipedia.py)
extras_test: tools/extras_test.c $(ENGINE_SRC) engine/cdie.h host/cdi_extras.h host/gameinfo.h build/libbearssl.a build/liblzma7z.a
	$(CC) $(CFLAGS) $(NFS_CFLAGS) $(LZMA_FLAGS) -Iengine -Ihost -Iinclude -I$(BEAR_DIR)/inc -o $@ tools/extras_test.c $(filter-out engine/cdi_engine.c,$(ENGINE_SRC)) build/liblzma7z.a build/libbearssl.a $(NFS_LIBS) -ldl -lpthread

keep_test: tools/keep_test.c $(ENGINE_SRC) engine/cdie.h build/libbearssl.a build/liblzma7z.a
	$(CC) $(CFLAGS) -fsanitize=address,undefined $(NFS_CFLAGS) $(LZMA_FLAGS) -Iengine -Ihost -Iinclude -I$(BEAR_DIR)/inc -o $@ tools/keep_test.c $(filter-out engine/cdi_engine.c,$(ENGINE_SRC)) build/liblzma7z.a build/libbearssl.a $(NFS_LIBS) -ldl -lpthread

testcore.so: tools/testcore.c include/libretro.h
	$(CC) $(CFLAGS) -fPIC -shared -Iinclude -o $@ tools/testcore.c -lm

# Headless smoke test: no display or sound card needed.
test: all
	echo x > /tmp/testcore.bin
	# 51200 Hz: SDL's dummy audio device keeps exact time only when samples*1000/rate is a whole number of ms
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TESTCORE_RATE=51200 ./cdihost ./testcore.so /tmp/testcore.bin --frames 300

clean:
	rm -f cdihost cdie_sdl testcore.so

clean-all: clean
	rm -rf build

.PHONY: all test clean clean-all
