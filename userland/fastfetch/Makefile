# SPDX-License-Identifier: MIT
# Makefile for fastfetch — OpenASD userland port (minimal link, no libasd stdio)

CC      = clang
CFLAGS  = --target=x86_64-apple-macosx13.0 \
          -ffreestanding \
          -fno-stack-protector \
          -fno-builtin \
          -mno-red-zone \
          -mgeneral-regs-only \
          -nostdlib \
          -nostdinc \
          -Os \
          -Wall \
          -Wextra \
          -Wno-macro-redefined \
          -I../libasd/include \
          -I$(shell $(CC) -print-resource-dir)/include

LD      = ld64.lld
LDFLAGS = -arch x86_64 \
          -platform_version macos 13.0 13.0 \
          -e _start -no_fixup_chains

BUILD   = build
TARGET  = $(BUILD)/fastfetch
START   = ../libasd/build/start.o
SYSCALL = ../libasd/build/syscall_asm.o

OBJS    = $(BUILD)/fastfetch.o

.PHONY: all clean

all: $(TARGET)

$(START) $(SYSCALL):
	$(MAKE) -C ../libasd

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(TARGET): $(OBJS) $(START) $(SYSCALL)
	$(LD) $(LDFLAGS) -o $@ $(START) $(OBJS) $(SYSCALL)

clean:
	rm -rf $(BUILD)
