# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, ASD Project Contributors
#
# asdsh — userspace shell, compiled as freestanding x86-64 ELF

CC      = clang
AS      = clang
CFLAGS  = --target=x86_64-apple-macosx13.0 \
          -ffreestanding \
          -fno-stack-protector \
          -fno-builtin \
          -mno-red-zone \
          -mno-sse -mno-sse2 -mno-avx \
          -nostdlib \
          -nostdinc \
          -I../libasd/include \
          -I$(shell clang -print-resource-dir)/include \
          -Os -Wall -Wextra -Wno-unused-parameter \
          -Wno-macro-redefined

LD      = ld64.lld
LDFLAGS = -arch x86_64 \
          -platform_version macos 13.0 13.0 \
          -e _start -no_fixup_chains

LIBASD  = ../libasd/build/libasd.a
START_OBJ = ../libasd/build/start.o

BUILD   = build
TARGET  = $(BUILD)/asdsh

.PHONY: all clean libasd

all: libasd $(TARGET)

libasd:
	$(MAKE) -C ../libasd

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/sh.o: sh.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(TARGET): $(BUILD)/sh.o $(LIBASD) $(START_OBJ) | $(BUILD)
	$(LD) $(LDFLAGS) $(START_OBJ) $(BUILD)/sh.o $(LIBASD) -o $@

clean:
	rm -rf $(BUILD)
