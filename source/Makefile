# Game Library Manager - PS5 payload
#   make            -> game-library-manager.elf (needs PS5_PAYLOAD_SDK)
#   make host       -> glm-host (Linux build for testing)
#
# tile/eboot.bin is the tiny fake-signed launcher for the home-screen tile;
# it is embedded in game-library-manager.elf, which installs it.
PS5_HOST ?= ps5
PS5_PORT ?= 9021

SRCS := src/main.c src/util.c src/config.c src/platform.c src/smp.c \
        src/imgpeek.c src/scan.c src/copy.c src/queue.c src/httpd.c \
        src/tile.c src/archive.c src/web_index.c src/blob_tile.c \
        src/crypto.c src/pkginfo.c

LZMA := third_party/lzma
LZMA_SRCS := $(addprefix $(LZMA)/,7zArcIn.c 7zBuf.c 7zCrc.c 7zCrcOpt.c \
        7zDec.c 7zStream.c 7zAlloc.c Alloc.c CpuArch.c Lzma2Dec.c \
        Lzma2DecMt.c LzmaDec.c MtDec.c Threads.c Bcj2.c Bra.c Bra86.c Delta.c)

TILE_AUTHID := 0x3800000000000022
TILE_SDKVER := 0x07590001

all: game-library-manager.elf

src/web_index.c: web/index.html tools/gen_web.py
	python3 tools/gen_web.py $< $@

src/blob_tile.c: tile/icon0.png tools/gen_blob.py
	python3 tools/gen_blob.py $@ tile_icon=tile/icon0.png


ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
LIBS := -lSceNet -lSceSsl -lSceHttp2 -lSceUserService -lSceSystemService -lSceAppInstUtil -lSceIpmi -lpthread

tile/eboot.o: tile/eboot.c
	$(CC) -c -O2 -Wall -Werror -ffreestanding -fno-stack-protector -fno-builtin -o $@ $<

tile/eboot.elf: tile/eboot.o tile/eboot.x
	$(LD) --static -T tile/eboot.x -o $@ tile/eboot.o

tile/eboot.bin: tile/eboot.elf tile/make_fself.py tile/authinfo.txt
	python3 tile/make_fself.py --ptype fake --paid $(TILE_AUTHID) \
	  --auth-info "$$(cat tile/authinfo.txt)" \
	  --app-version $(TILE_SDKVER) --fw-version $(TILE_SDKVER) $< $@

game-library-manager.elf: $(SRCS) src/common.h src/crypto.h lzma-ps5.a
	$(CC) -O2 -Wall -Werror -Isrc -I$(LZMA) -o $@ $(SRCS) .lzma-ps5/*.o $(LIBS)

lzma-ps5.a: $(LZMA_SRCS)
	rm -rf .lzma-ps5 && mkdir -p .lzma-ps5
	cd .lzma-ps5 && for f in $(LZMA_SRCS); do $(CC) -O2 -c ../$$f || exit 1; done
	$(AR) rcs $@ .lzma-ps5/*.o
	$(STRIP) $@ 2>/dev/null || true

test: game-library-manager.elf
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^
else
game-library-manager.elf tile/eboot.bin:
	$(error PS5_PAYLOAD_SDK is undefined - export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk)
endif

host: glm-host
glm-host: $(SRCS) src/common.h src/crypto.h lzma-host.a
	cc -O2 -g -Wall -Werror -Wno-format-truncation -Wno-stringop-truncation \
	   -Isrc -I$(LZMA) -o $@ $(SRCS) lzma-host.a -lpthread

lzma-host.a: $(LZMA_SRCS)
	rm -rf .lzma-host && mkdir -p .lzma-host
	cd .lzma-host && for f in $(LZMA_SRCS); do cc -O2 -c ../$$f || exit 1; done
	ar rcs $@ .lzma-host/*.o

clean:
	rm -rf game-library-manager.elf glm-host lzma-*.a .lzma-* tile/*.o tile/eboot.elf \
	      src/blob_*.c

.PHONY: all host clean test

# Linux build of the tile launcher, for testing its logic
tile-test: tile/eboot.c
	mkdir -p /tmp/ss-app0
	cc -O1 -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin \
	   -DLINUX_TEST='"/tmp/ss-app0"' -o tile/eboot-linux tile/eboot.c
