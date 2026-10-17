# Cross-build for the AP80 Pro-X (MIPS32r2, static). Run inside the ap80-tc Docker image.
CC = mipsel-linux-gnu-gcc
TA = third_party/tinyalsa
CFLAGS = -march=mips32r2 -O2 -Wall -Wno-format-truncation -Wno-unused-function -Wno-misleading-indentation -I$(TA)/include -D_GNU_SOURCE
LDFLAGS = -static -s -lpthread -lm -ldl
SRC = src/main.c src/gfx.c src/hw.c src/audio.c src/usb.c $(TA)/src/pcm.c $(TA)/src/pcm_hw.c $(TA)/src/mixer.c $(TA)/src/mixer_hw.c $(TA)/src/limits.c $(TA)/src/snd_card_plugin.c

build/ap80player: $(SRC) src/*.h
	mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -rf build
