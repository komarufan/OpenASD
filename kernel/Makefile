# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, ASD Project Contributors
#
# Build the ASD kernel — freestanding x86-64 binary

CC      = clang
CFLAGS  = --target=x86_64-unknown-elf \
          -ffreestanding \
          -fno-builtin \
          -fno-stack-protector \
          -mno-red-zone \
          -nostdlib \
          -Wall \
          -Wextra \
          -Wno-unused-function \
          -Wno-missing-field-initializers \
          -Os \
          -I. \
          -I../init \
          -Iinclude \
          -I$(shell $(CC) -print-resource-dir)/include

AS      = clang
ASFLAGS = --target=x86_64-unknown-elf -ffreestanding -c

LD      = ld.lld
LDFLAGS = -m elf_x86_64 \
          -nostdlib \
          -static \
          -Ttext=0x4000000 \
          --entry=kernel_main

# Build directory
BUILD   = build

# Object files (all placed in $(BUILD)/)
OBJS = \
    $(BUILD)/entry.o \
    $(BUILD)/fbcon.o \
    $(BUILD)/string.o \
    $(BUILD)/mm.o \
    $(BUILD)/block.o \
    $(BUILD)/gpt.o \
    $(BUILD)/sched.o \
    $(BUILD)/sched_asm.o \
    $(BUILD)/port.o \
    $(BUILD)/vfs.o \
    $(BUILD)/fat32.o \
    $(BUILD)/ffs.o \
    $(BUILD)/ramfs.o \
    $(BUILD)/adf.o \
    $(BUILD)/virtio_blk.o \
    $(BUILD)/virtio_net.o \
    $(BUILD)/net.o \
    $(BUILD)/tls.o \
    $(BUILD)/usr.o \
    $(BUILD)/asdinit.o \
    $(BUILD)/init_io.o \
    $(BUILD)/init_installer.o \
    $(BUILD)/init_installer_tui.o \
    $(BUILD)/init_shell.o \
    $(BUILD)/init_svcmgr.o \
    $(BUILD)/gdt.o \
    $(BUILD)/idt.o \
    $(BUILD)/isr.o \
    $(BUILD)/pic.o \
    $(BUILD)/pit.o \
    $(BUILD)/ps2kbd.o \
    $(BUILD)/ps2mouse.o \
    $(BUILD)/syscall.o \
    $(BUILD)/syscall_entry.o \
    $(BUILD)/macho.o \
    $(BUILD)/security.o

TARGET = $(BUILD)/asdkernel.bin

.PHONY: all clean

all: $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

# Compile kernel source files
$(BUILD)/entry.o: entry.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/fbcon.o: console/fbcon.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/string.o: lib/string.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/mm.o: mm/mm.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/block.o: block/block.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/gpt.o: block/gpt.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/sched.o: sched/sched.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/sched_asm.o: sched/sched_asm.S  | $(BUILD)
	$(AS) $(ASFLAGS) -o $@ $<

$(BUILD)/port.o: ipc/port.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/vfs.o: vfs/vfs.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/fat32.o: vfs/fat32.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/ffs.o: vfs/ffs.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/ramfs.o: vfs/ramfs.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/adf.o: drv/adf.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/virtio_blk.o: drv/virtio_blk.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/virtio_net.o: drv/virtio_net.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/net.o: net/net.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/tls.o: net/tls.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/usr.o: usr/usr.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# Compile asdinit and sub-files (linked into kernel for bring-up)
$(BUILD)/asdinit.o: ../init/asdinit.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/init_io.o: ../init/io.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/init_installer.o: ../init/installer.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/init_installer_tui.o: ../init/installer_tui.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/init_shell.o: ../init/shell.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/init_svcmgr.o: ../init/svcmgr.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# Arch subsystem
$(BUILD)/gdt.o: arch/gdt.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/idt.o: arch/idt.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/isr.o: arch/isr.S  | $(BUILD)
	$(AS) $(ASFLAGS) -o $@ $<

$(BUILD)/pic.o: arch/pic.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/pit.o: arch/pit.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/syscall.o: arch/syscall.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/syscall_entry.o: arch/syscall_entry.S  | $(BUILD)
	$(AS) $(ASFLAGS) -o $@ $<

$(BUILD)/macho.o: arch/macho.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/security.o: security/security.c  | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# Drivers
$(BUILD)/ps2kbd.o: drv/ps2kbd.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/ps2mouse.o: drv/ps2mouse.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

uki: $(TARGET)
	@echo "UKI build: use 'make uki' in root directory"

clean:
	rm -rf $(BUILD)
