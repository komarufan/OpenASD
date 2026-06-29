CC      = clang
CFLAGS  = --target=x86_64-apple-macosx13.0 \
          -ffreestanding \
          -fno-stack-protector \
          -fno-builtin \
          -mno-red-zone \
          -mno-sse -mno-sse2 -mno-avx \
          -nostdlib \
          -nostdinc \
          -I../libasd/include \
          -I../libgui \
          -I$(shell clang -print-resource-dir)/include \
          -Os -Wall -Wextra

LD      = ld64.lld
LDFLAGS = -arch x86_64 \
          -platform_version macos 13.0 13.0 \
          -e _start -no_fixup_chains

LIBASD  = ../libasd/build/libasd.a
LIBGUI  = ../libgui/build/libgui.a
START_OBJ = ../libasd/build/start.o

BUILD   = build
TARGET  = $(BUILD)/ws

.PHONY: all clean libasd libgui

all: libasd libgui $(TARGET)

libasd:
	$(MAKE) -C ../libasd

libgui:
	$(MAKE) -C ../libgui

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/main.o: main.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/wallpaper.o: wallpaper.S wallpaper.bin | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ wallpaper.S

$(TARGET): $(BUILD)/main.o $(BUILD)/wallpaper.o $(LIBASD) $(LIBGUI) $(START_OBJ) | $(BUILD)
	$(LD) $(LDFLAGS) $(START_OBJ) $(BUILD)/main.o $(BUILD)/wallpaper.o $(LIBGUI) $(LIBASD) -o $@

clean:
	rm -rf $(BUILD)
