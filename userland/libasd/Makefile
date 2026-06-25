# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, ASD Project Contributors
#
# libasd — freestanding C library for ASD userland (x86-64 bare-metal Mach-O)

CC     = clang
AS     = clang
CFLAGS = --target=x86_64-apple-macosx13.0 \
         -ffreestanding \
         -fno-stack-protector \
         -fno-builtin \
         -mno-red-zone \
         -mno-sse -mno-sse2 -mno-avx \
         -mgeneral-regs-only \
         -nostdlib \
         -nostdinc \
         -Iinclude \
         -I$(shell clang -print-resource-dir)/include \
         -Os -Wall -Wextra -Wno-unused-parameter \
          -Wno-macro-redefined

ASFLAGS = --target=x86_64-apple-macosx13.0 -ffreestanding -c

BUILD = build
LIB   = $(BUILD)/libasd.a

OBJS  = $(BUILD)/start.o \
        $(BUILD)/syscall_asm.o \
        $(BUILD)/stdio.o \
        $(BUILD)/string.o

.PHONY: all clean rust

all: $(LIB) rust

# Locate cargo at parse time (wildcard works at make parse time, not in recipes)
_CARGO_SEARCH := /c/Users/Администратор/.cargo/bin/cargo \
                 /c/Users/Administrator/.cargo/bin/cargo \
                 $(HOME)/.cargo/bin/cargo
_CARGO_FOUND  := $(firstword $(wildcard $(_CARGO_SEARCH)))

rust: | $(BUILD)
	@if [ -n "$(_CARGO_FOUND)" ]; then \
		$(MAKE) -C rust; \
	else \
		echo "  [skip] Rust/cargo not found — skipping asd_sys crate (install rustup to enable)"; \
	fi


$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/start.o: start.S | $(BUILD)
	$(AS) $(ASFLAGS) -o $@ $<

$(BUILD)/syscall_asm.o: syscall_asm.S | $(BUILD)
	$(AS) $(ASFLAGS) -o $@ $<

$(BUILD)/stdio.o: stdio.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/string.o: string.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(LIB): $(OBJS)
	ar rcs $@ $(OBJS)

clean:
	rm -rf $(BUILD)
	$(MAKE) -C rust clean
