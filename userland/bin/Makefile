CC := clang
LD := ld64.lld

CFLAGS  = --target=x86_64-apple-macosx13.0 -nostdlib -nostdinc -ffreestanding \
          -fno-stack-protector -fno-builtin -mno-red-zone \
          -mno-sse -mno-sse2 -mno-avx \
          -I../libasd/include -I. -I$(shell $(CC) -print-resource-dir)/include \
          -Wno-macro-redefined -Os -Wall -Wextra
LDFLAGS = -arch x86_64 \
          -platform_version macos 13.0 13.0 \
          -e _start -no_fixup_chains

LIBASD = ../libasd/build/libasd.a
START_OBJ = ../libasd/build/start.o

BINS = ls cat mkdir rm touch echo pwd sysinfo uname uptime id whoami kill hexdump wc ping filetest nettest hxtest \
       grep find sort head tail do apm
BIN_TARGETS = $(addprefix build/,$(BINS))

.PHONY: all clean libasd

all: libasd $(BIN_TARGETS)

libasd:
	$(MAKE) -C ../libasd

build:
	mkdir -p build

build/%.o: %.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/%: build/%.o $(LIBASD) $(START_OBJ) | build
	$(LD) $(LDFLAGS) $(START_OBJ) $< $(LIBASD) -o $@

clean:
	rm -rf build
