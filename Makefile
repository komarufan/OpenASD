# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, ASD Project Contributors

.PHONY: all boot kernel userland uki live-image prepare-run run run-debug run-headless-debug run-gui-debug bfd bfd-debug bfd-autotest bfd-hxtest bfd-nettest iso usb-image clean install install-fresh

QEMU ?= qemu-system-x86_64
QEMU_DISPLAY ?= gtk
QEMU_VIDEO_ARGS ?= -device virtio-vga
RUN_DIR := build/run
EFI_DIR := $(RUN_DIR)/EFI/BOOT
BOOT_DIR := $(RUN_DIR)/boot
BUILD_DIR := build
DISK_DIR := $(BUILD_DIR)/disk
DISK_BOOT_DIR := $(DISK_DIR)/boot
DISK_EFI_DIR := $(DISK_DIR)/EFI/BOOT
DISK_IMG := $(DISK_DIR)/asd-target.img
LIVE_IMG := $(RUN_DIR)/live.img
# Try common OVMF paths
OVMF_CODE ?= $(shell for f in \
	/usr/share/edk2/x64/OVMF_CODE.4m.fd \
	/usr/share/OVMF/OVMF_CODE.fd \
	/usr/share/qemu/OVMF.fd \
	/usr/share/edk2-ovmf/x64/OVMF_CODE.fd \
	/ucrt64/share/qemu/edk2-x86_64-code.fd; \
	do if [ -f "$$f" ]; then echo "$$f"; break; fi; done)

OVMF_VARS ?= $(shell for f in \
	/usr/share/edk2/x64/OVMF_VARS.4m.fd \
	/usr/share/OVMF/OVMF_VARS.fd \
	/usr/share/qemu/OVMF_VARS.fd \
	/usr/share/edk2-ovmf/x64/OVMF_VARS.fd \
	/ucrt64/share/qemu/edk2-i386-vars.fd; \
	do if [ -f "$$f" ]; then echo "$$f"; break; fi; done)
OVMF_VARS_RUN := $(RUN_DIR)/OVMF_VARS.fd
OVMF_VARS_BFD := $(DISK_DIR)/OVMF_VARS.fd
DEBUG_DIR := $(BUILD_DIR)/debug
SERIAL_LOG := $(DEBUG_DIR)/serial.log
QEMU_DEBUG_LOG := $(DEBUG_DIR)/qemu.log
ISO_DIR  := $(BUILD_DIR)/iso
ISO_IMG  := $(BUILD_DIR)/asd.iso
EFI_IMG  := $(ISO_DIR)/EFI/efiboot.img
VERSION  := 1.0
USB_DIR  := $(BUILD_DIR)/usb
USB_IMG  := $(USB_DIR)/openasd-$(VERSION).img
USB_SIZE_MB := 128

# Shared boot config template — written to $(1)
define write-asdboot-conf
	@printf '%s\n' \
		'timeout = 0' \
		'' \
		'menu' \
		'  title = "ASD Boot"' \
		'  default = "default"' \
		'end' \
		'' \
		'entry' \
		'  id = "default"' \
		'  label = "ASD Kernel"' \
		'  kernel = "/boot/asdkernel.bin"' \
		'  cmdline = ""' \
		'end' > $(1)
endef

# Check mtools + dosfstools are present
define check-mtools
	@for t in mkfs.vfat mmd mcopy; do \
		command -v $$t >/dev/null || { \
			echo "Missing: $$t — run: sudo pacman -S mtools dosfstools"; exit 1; }; \
	done
endef

define check-ovmf
	@if [ -z "$(OVMF_CODE)" ] || [ ! -f "$(OVMF_CODE)" ]; then \
		echo "Error: OVMF_CODE not found. Install edk2-ovmf/ovmf or set OVMF_CODE=..."; \
		exit 1; \
	fi
	@if [ -z "$(OVMF_VARS)" ] || [ ! -f "$(OVMF_VARS)" ]; then \
		echo "Error: OVMF_VARS not found. Install edk2-ovmf/ovmf or set OVMF_VARS=..."; \
		exit 1; \
	fi
endef

# Bins to package in /bin on live image / install disk
LIVE_BINS = ls cat mkdir rm touch echo pwd sysinfo uname uptime id whoami kill hexdump wc ping filetest nettest hxtest \
            grep find sort head tail do apm

all:
	$(MAKE) -C boot
	$(MAKE) -C kernel
	$(MAKE) -C userland

boot:
	$(MAKE) -C boot

kernel:
	$(MAKE) -C kernel

userland:
	$(MAKE) -C userland

uki: kernel
	$(MAKE) -C kernel uki

# Rebuild fastfetch when kernel syscall/elf loader changes (spawn path).
userland/mifetch/build/mifetch: kernel/arch/syscall.c kernel/arch/macho.c userland/mifetch/mifetch.c
	$(MAKE) -C userland/mifetch

live-image: all
	$(call check-mtools)
	@mkdir -p "$(EFI_DIR)" "$(BOOT_DIR)"
	@cp boot/asdboot.efi              "$(EFI_DIR)/BOOTX64.EFI"
	@cp kernel/build/asdkernel.bin   "$(EFI_DIR)/asdkernel.bin"
	@cp kernel/build/asdkernel.bin   "$(BOOT_DIR)/asdkernel.bin"
	$(call write-asdboot-conf,"$(EFI_DIR)/asdboot.conf")
	@cp "$(EFI_DIR)/asdboot.conf"    "$(BOOT_DIR)/asdboot.conf"
	@dd if=/dev/zero of="$(LIVE_IMG)" bs=1M count=64 2>/dev/null
	@mkfs.vfat -F 32 "$(LIVE_IMG)" >/dev/null
	@mmd  -i "$(LIVE_IMG)" ::/EFI ::/EFI/BOOT ::/boot ::/bin ::/sbin
	@mcopy -i "$(LIVE_IMG)" "$(EFI_DIR)/BOOTX64.EFI"    ::/EFI/BOOT/BOOTX64.EFI
	@mcopy -i "$(LIVE_IMG)" "$(EFI_DIR)/asdkernel.bin"  ::/EFI/BOOT/asdkernel.bin
	@mcopy -i "$(LIVE_IMG)" "$(EFI_DIR)/asdboot.conf"   ::/EFI/BOOT/asdboot.conf
	@mcopy -i "$(LIVE_IMG)" "$(BOOT_DIR)/asdkernel.bin" ::/boot/asdkernel.bin
	@mcopy -i "$(LIVE_IMG)" "$(BOOT_DIR)/asdboot.conf"  ::/boot/asdboot.conf
	@mcopy -i "$(LIVE_IMG)" userland/sh/build/asdsh ::/bin/asdsh
	@for b in $(LIVE_BINS); do \
		mcopy -i "$(LIVE_IMG)" userland/bin/build/$$b ::/bin/$$b; \
	done
	@[ -f userland/hx/build/hx ] && mcopy -i "$(LIVE_IMG)" userland/hx/build/hx ::/bin/hx || true
	@mcopy -i "$(LIVE_IMG)" userland/sbin/build/asdlog          ::/sbin/asdlog
	@mcopy -i "$(LIVE_IMG)" userland/sbin/build/netd            ::/sbin/netd
	@echo "==> $(LIVE_IMG) ($$(du -h $(LIVE_IMG) | cut -f1))"

prepare-run: all
	$(call check-mtools)
	$(call check-ovmf)
	@mkdir -p "$(EFI_DIR)" "$(BOOT_DIR)"
	@cp boot/asdboot.efi              "$(EFI_DIR)/BOOTX64.EFI"
	@cp kernel/build/asdkernel.bin   "$(EFI_DIR)/asdkernel.bin"
	@cp kernel/build/asdkernel.bin   "$(BOOT_DIR)/asdkernel.bin"
	$(call write-asdboot-conf,"$(EFI_DIR)/asdboot.conf")
	@cp "$(EFI_DIR)/asdboot.conf"    "$(BOOT_DIR)/asdboot.conf"
	@dd if=/dev/zero of="$(LIVE_IMG)" bs=1M count=64 2>/dev/null
	@mkfs.vfat -F 32 "$(LIVE_IMG)" >/dev/null
	@mmd  -i "$(LIVE_IMG)" ::/EFI ::/EFI/BOOT ::/boot ::/bin ::/sbin
	@mcopy -i "$(LIVE_IMG)" "$(EFI_DIR)/BOOTX64.EFI"    ::/EFI/BOOT/BOOTX64.EFI
	@mcopy -i "$(LIVE_IMG)" "$(EFI_DIR)/asdkernel.bin"  ::/EFI/BOOT/asdkernel.bin
	@mcopy -i "$(LIVE_IMG)" "$(EFI_DIR)/asdboot.conf"   ::/EFI/BOOT/asdboot.conf
	@mcopy -i "$(LIVE_IMG)" "$(BOOT_DIR)/asdkernel.bin" ::/boot/asdkernel.bin
	@mcopy -i "$(LIVE_IMG)" "$(BOOT_DIR)/asdboot.conf"  ::/boot/asdboot.conf
	@mcopy -i "$(LIVE_IMG)" userland/sh/build/asdsh ::/bin/asdsh
	@for b in $(LIVE_BINS); do \
		mcopy -i "$(LIVE_IMG)" userland/bin/build/$$b ::/bin/$$b; \
	done
	@[ -f userland/hx/build/hx ] && mcopy -i "$(LIVE_IMG)" userland/hx/build/hx ::/bin/hx || true
	@mcopy -i "$(LIVE_IMG)" userland/sbin/build/asdlog          ::/sbin/asdlog
	@mcopy -i "$(LIVE_IMG)" userland/sbin/build/netd            ::/sbin/netd
	@printf '#ASDPW1\n' > "$(RUN_DIR)/asdpw1.stub"
	@mcopy -i "$(LIVE_IMG)" "$(RUN_DIR)/asdpw1.stub" ::/ASDPW1
	@cp "$(OVMF_VARS)" "$(OVMF_VARS_RUN)"
	@mkdir -p "$(DISK_DIR)"
	@if [ ! -f "$(DISK_IMG)" ]; then \
		qemu-img create -f raw "$(DISK_IMG)" 2G >/dev/null; \
	fi

run: prepare-run
	$(QEMU) \
		-machine q35,accel=kvm:tcg \
		-cpu qemu64 \
		-m 1024 \
		$(QEMU_VIDEO_ARGS) \
		-display $(QEMU_DISPLAY) \
		-drive if=pflash,format=raw,readonly=on,file="$(OVMF_CODE)" \
		-drive if=pflash,format=raw,file="$(OVMF_VARS_RUN)" \
		-drive file="$(LIVE_IMG)",if=none,id=live0,format=raw \
		-device virtio-blk-pci,drive=live0,bootindex=1 \
		-drive file="$(DISK_IMG)",if=none,id=inst0,format=raw \
		-device virtio-blk-pci,drive=inst0,bootindex=2 \
		-netdev user,id=net0,net=10.0.2.0/24,host=10.0.2.2,dns=10.0.2.3 \
		-device virtio-net-pci,netdev=net0

# GUI + serial.log + qemu.log — reproduce installer, then share the log.
run-debug: prepare-run
	@mkdir -p "$(DEBUG_DIR)"
	@echo "OVMF_CODE=$(OVMF_CODE)"
	@echo "Display:    $(QEMU_DISPLAY)"
	@echo "Serial log: $(SERIAL_LOG)"
	@echo "QEMU log:   $(QEMU_DEBUG_LOG)"
	@echo "Stop QEMU with Ctrl+C, then: tail -80 $(SERIAL_LOG)"
	$(QEMU) \
		-machine q35,accel=kvm:tcg \
		-cpu qemu64 \
		-m 1024 \
		$(QEMU_VIDEO_ARGS) \
		-display $(QEMU_DISPLAY) \
		-no-reboot \
		-serial file:$(SERIAL_LOG) \
		-d guest_errors,int,cpu_reset \
		-D $(QEMU_DEBUG_LOG) \
		-drive if=pflash,format=raw,readonly=on,file="$(OVMF_CODE)" \
		-drive if=pflash,format=raw,file="$(OVMF_VARS_RUN)" \
		-drive file="$(LIVE_IMG)",if=none,id=live0,format=raw \
		-device virtio-blk-pci,drive=live0,bootindex=1 \
		-drive file="$(DISK_IMG)",if=none,id=inst0,format=raw \
		-device virtio-blk-pci,drive=inst0,bootindex=2 \
		-netdev user,id=net0,net=10.0.2.0/24,host=10.0.2.2,dns=10.0.2.3 \
		-device virtio-net-pci,netdev=net0

# Headless: serial.log + qemu.log only (CI / WSL without display).
run-headless-debug: prepare-run
	@mkdir -p "$(DEBUG_DIR)"
	@echo "Serial log: $(SERIAL_LOG)"
	@echo "QEMU log:   $(QEMU_DEBUG_LOG)"
	@echo "Stop QEMU with Ctrl+C, then: tail -80 $(SERIAL_LOG)"
	$(QEMU) \
		-machine q35,accel=tcg \
		-cpu qemu64 \
		-m 1024 \
		-display none \
		-no-reboot \
		-serial file:$(SERIAL_LOG) \
		-d guest_errors,int,cpu_reset \
		-D $(QEMU_DEBUG_LOG) \
		-drive if=pflash,format=raw,readonly=on,file="$(OVMF_CODE)" \
		-drive if=pflash,format=raw,file="$(OVMF_VARS_RUN)" \
		-drive file="$(LIVE_IMG)",if=none,id=live0,format=raw \
		-device virtio-blk-pci,drive=live0,bootindex=1 \
		-drive file="$(DISK_IMG)",if=none,id=inst0,format=raw \
		-device virtio-blk-pci,drive=inst0,bootindex=2 \
		-netdev user,id=net0,net=10.0.2.0/24,host=10.0.2.2,dns=10.0.2.3 \
		-device virtio-net-pci,netdev=net0

# Alias for run-debug (kept for older docs/scripts).
run-gui-debug: run-debug

# Wipe install disk and lay down a clean FAT32 (use after GPT corruption / bfd issues).
install-fresh:
	@rm -f "$(DISK_IMG)"
	@$(MAKE) install

install: all
	$(call check-mtools)
	@mkdir -p "$(DISK_EFI_DIR)" "$(DISK_BOOT_DIR)" "$(RUN_DIR)"
	@cp boot/asdboot.efi              "$(DISK_EFI_DIR)/BOOTX64.EFI"
	@cp kernel/build/asdkernel.bin   "$(DISK_EFI_DIR)/asdkernel.bin"
	@cp kernel/build/asdkernel.bin   "$(DISK_BOOT_DIR)/asdkernel.bin"
	$(call write-asdboot-conf,"$(DISK_EFI_DIR)/asdboot.conf")
	@cp "$(DISK_EFI_DIR)/asdboot.conf"    "$(DISK_BOOT_DIR)/asdboot.conf"
	@if [ ! -f "$(DISK_IMG)" ]; then \
		qemu-img create -f raw "$(DISK_IMG)" 2G >/dev/null; \
		mkfs.vfat -F 32 "$(DISK_IMG)" >/dev/null; \
	else \
		if dd if="$(DISK_IMG)" bs=1 skip=512 count=8 2>/dev/null | grep -aq 'EFI PART'; then \
			echo "  $(DISK_IMG): GPT/OpenASD layout kept (no mkfs)"; \
		elif ! minfo -i "$(DISK_IMG)" :: >/dev/null 2>&1; then \
			mkfs.vfat -F 32 "$(DISK_IMG)" >/dev/null; \
		fi; \
	fi
	@mmd -i "$(DISK_IMG)" ::/EFI ::/EFI/BOOT ::/boot ::/bin ::/sbin 2>/dev/null || true
	@mcopy -o -i "$(DISK_IMG)" "$(DISK_EFI_DIR)/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
	@mcopy -o -i "$(DISK_IMG)" "$(DISK_EFI_DIR)/asdkernel.bin" ::/EFI/BOOT/asdkernel.bin
	@mcopy -o -i "$(DISK_IMG)" "$(DISK_EFI_DIR)/asdboot.conf" ::/EFI/BOOT/asdboot.conf
	@mcopy -o -i "$(DISK_IMG)" "$(DISK_BOOT_DIR)/asdkernel.bin" ::/boot/asdkernel.bin
	@mcopy -o -i "$(DISK_IMG)" "$(DISK_BOOT_DIR)/asdboot.conf" ::/boot/asdboot.conf
	@mcopy -o -i "$(DISK_IMG)" userland/sh/build/asdsh ::/bin/asdsh
	@for b in $(LIVE_BINS); do \
		mcopy -o -i "$(DISK_IMG)" userland/bin/build/$$b ::/bin/$$b; \
	done
	@[ -f userland/hx/build/hx ] && mcopy -o -i "$(DISK_IMG)" userland/hx/build/hx ::/bin/hx || true
	@mcopy -o -i "$(DISK_IMG)" userland/sbin/build/asdlog ::/sbin/asdlog
	@mcopy -o -i "$(DISK_IMG)" userland/sbin/build/netd   ::/sbin/netd
	@printf '#ASDPW1\n' > "$(RUN_DIR)/asdpw1.stub"
	@mcopy -i "$(DISK_IMG)" "$(RUN_DIR)/asdpw1.stub" ::/ASDPW1
	@echo "Installation to $(DISK_IMG) complete."

bfd: all
	@if [ ! -f "$(DISK_IMG)" ]; then \
		echo "No $(DISK_IMG). Run 'make install' once, or install via QEMU (make run)."; \
		exit 1; \
	fi
	@if [ ! -f "$(OVMF_CODE)" ] || [ ! -f "$(OVMF_VARS)" ]; then \
		echo "OVMF firmware not found."; \
		exit 1; \
	fi
	@if [ ! -f "$(OVMF_VARS_BFD)" ]; then \
		cp "$(OVMF_VARS)" "$(OVMF_VARS_BFD)"; \
	fi
	$(QEMU) \
		-machine q35,accel=kvm:tcg \
		-cpu qemu64 \
		-m 1024 \
		-no-reboot \
		$(QEMU_VIDEO_ARGS) \
		-display $(QEMU_DISPLAY) \
		-drive if=pflash,format=raw,readonly=on,file="$(OVMF_CODE)" \
		-drive if=pflash,format=raw,file="$(OVMF_VARS_BFD)" \
		-drive file="$(DISK_IMG)",if=none,id=inst0,format=raw \
		-device virtio-blk-pci,drive=inst0 \
		-netdev user,id=net0,net=10.0.2.0/24,host=10.0.2.2,dns=10.0.2.3 \
		-device virtio-net-pci,netdev=net0

# Installed disk only: GUI (login + shell in window) + serial.log + qemu.log.
bfd-debug: all
	@mkdir -p "$(DEBUG_DIR)"
	@echo "Display:    $(QEMU_DISPLAY)  (override: make bfd-debug QEMU_DISPLAY=sdl)"
	@echo "Serial log: $(SERIAL_LOG)"
	@echo "QEMU log:   $(QEMU_DEBUG_LOG)"
	@echo "Use the QEMU window for login/shell; serial is also written to the log file."
	@echo "Stop QEMU with Ctrl+C, then: grep -E 'EXCEPTION|fastfetch' $(SERIAL_LOG)"
	@if [ ! -f "$(OVMF_CODE)" ] || [ ! -f "$(OVMF_VARS)" ]; then \
		echo "OVMF firmware not found."; \
		exit 1; \
	fi
	@if [ ! -f "$(DISK_IMG)" ]; then \
		echo "No $(DISK_IMG) — run: make install"; \
		exit 1; \
	fi
	@if [ ! -f "$(OVMF_VARS_BFD)" ]; then \
		cp "$(OVMF_VARS)" "$(OVMF_VARS_BFD)"; \
	fi
	$(QEMU) \
		-machine q35,accel=kvm:tcg \
		-cpu qemu64 \
		-m 1024 \
		-no-reboot \
		$(QEMU_VIDEO_ARGS) \
		-display $(QEMU_DISPLAY) \
		-serial file:$(SERIAL_LOG) \
		-d guest_errors,int,cpu_reset \
		-D $(QEMU_DEBUG_LOG) \
		-drive if=pflash,format=raw,readonly=on,file="$(OVMF_CODE)" \
		-drive if=pflash,format=raw,file="$(OVMF_VARS_BFD)" \
		-drive file="$(DISK_IMG)",if=none,id=inst0,format=raw \
		-device virtio-blk-pci,drive=inst0 \
		-netdev user,id=net0,net=10.0.2.0/24,host=10.0.2.2,dns=10.0.2.3 \
		-device virtio-net-pci,netdev=net0

# Headless: boot with cmdline autotest_fastfetch, run /bin/fastfetch, halt.
define write-asdboot-conf-autotest
	@printf '%s\n' \
		'timeout = 0' \
		'' \
		'menu' \
		'  title = "ASD Boot"' \
		'  default = "default"' \
		'end' \
		'' \
		'entry' \
		'  id = "default"' \
		'  label = "ASD Kernel"' \
		'  kernel = "/boot/asdkernel.bin"' \
		'  cmdline = "autotest_fastfetch"' \
		'end' > $(1)
endef

define write-asdboot-conf-nettest
	@printf '%s\n' \
		'timeout = 0' \
		'' \
		'menu' \
		'  title = "ASD Boot"' \
		'  default = "default"' \
		'end' \
		'' \
		'entry' \
		'  id = "default"' \
		'  label = "ASD Kernel"' \
		'  kernel = "/boot/asdkernel.bin"' \
		'  cmdline = "autotest_nettest"' \
		'end' > $(1)
endef

define write-asdboot-conf-hxtest
	@printf '%s\n' \
		'timeout = 0' \
		'' \
		'menu' \
		'  title = "ASD Boot"' \
		'  default = "default"' \
		'end' \
		'' \
		'entry' \
		'  id = "default"' \
		'  label = "ASD Kernel"' \
		'  kernel = "/boot/asdkernel.bin"' \
		'  cmdline = "autotest_hxtest"' \
		'end' > $(1)
endef

bfd-autotest: all
	$(call check-mtools)
	@mkdir -p "$(DEBUG_DIR)"
	@if [ ! -f "$(DISK_IMG)" ]; then $(MAKE) install; fi
	@if dd if="$(DISK_IMG)" bs=1 skip=512 count=8 2>/dev/null | grep -aq 'EFI PART'; then \
		echo "bfd-autotest: $(DISK_IMG) is GPT (installer layout); mcopy cannot patch it."; \
		echo "  Use: make bfd-debug   (GUI + serial.log, type fastfetch in the shell)"; \
		exit 1; \
	fi
	$(call write-asdboot-conf-autotest,"$(RUN_DIR)/asdboot-autotest.conf")
	@mcopy -i "$(DISK_IMG)" "$(RUN_DIR)/asdboot-autotest.conf" ::/EFI/BOOT/asdboot.conf
	@mcopy -i "$(DISK_IMG)" "$(RUN_DIR)/asdboot-autotest.conf" ::/boot/asdboot.conf
	@echo "Serial log: $(SERIAL_LOG)"
	@if [ ! -f "$(OVMF_CODE)" ] || [ ! -f "$(OVMF_VARS)" ]; then \
		echo "OVMF firmware not found."; exit 1; fi
	@if [ ! -f "$(OVMF_VARS_BFD)" ]; then cp "$(OVMF_VARS)" "$(OVMF_VARS_BFD)"; fi
	@rm -f "$(SERIAL_LOG)" "$(QEMU_DEBUG_LOG)"
	@echo "Running QEMU autotest (~25s)..."
	@( command -v timeout >/dev/null && timeout 25 $(QEMU) \
		-machine q35,accel=tcg -cpu qemu64 -m 1024 -display none -no-reboot \
		-serial file:$(SERIAL_LOG) \
		-d guest_errors,int,cpu_reset -D $(QEMU_DEBUG_LOG) \
		-drive if=pflash,format=raw,readonly=on,file="$(OVMF_CODE)" \
		-drive if=pflash,format=raw,file="$(OVMF_VARS_BFD)" \
		-drive file="$(DISK_IMG)",if=none,id=inst0,format=raw \
		-device virtio-blk-pci,drive=inst0 \
	) || $(QEMU) \
		-machine q35,accel=tcg -cpu qemu64 -m 1024 -display none -no-reboot \
		-serial file:$(SERIAL_LOG) \
		-d guest_errors,int,cpu_reset -D $(QEMU_DEBUG_LOG) \
		-drive if=pflash,format=raw,readonly=on,file="$(OVMF_CODE)" \
		-drive if=pflash,format=raw,file="$(OVMF_VARS_BFD)" \
		-drive file="$(DISK_IMG)",if=none,id=inst0,format=raw \
		-device virtio-blk-pci,drive=inst0 \
		-netdev user,id=net0,net=10.0.2.0/24,host=10.0.2.2,dns=10.0.2.3 \
		-device virtio-net-pci,netdev=net0
	@echo "--- serial.log (last 40 lines) ---"
	@tail -40 "$(SERIAL_LOG)" 2>/dev/null || true
	@if grep -q '\*\*\* EXCEPTION' "$(SERIAL_LOG)" 2>/dev/null; then \
		echo "FAIL: kernel exception in $(SERIAL_LOG)"; \
		mcopy -o -i "$(DISK_IMG)" "$(DISK_EFI_DIR)/asdboot.conf" ::/EFI/BOOT/asdboot.conf 2>/dev/null || true; \
		mcopy -o -i "$(DISK_IMG)" "$(DISK_EFI_DIR)/asdboot.conf" ::/boot/asdboot.conf 2>/dev/null || true; \
		exit 1; \
	elif grep -q '\[autotest\] OK' "$(SERIAL_LOG)" 2>/dev/null; then \
		echo "PASS: fastfetch autotest completed"; \
		mcopy -o -i "$(DISK_IMG)" "$(DISK_EFI_DIR)/asdboot.conf" ::/EFI/BOOT/asdboot.conf 2>/dev/null || true; \
		mcopy -o -i "$(DISK_IMG)" "$(DISK_EFI_DIR)/asdboot.conf" ::/boot/asdboot.conf 2>/dev/null || true; \
	else \
		echo "WARN: no [autotest] OK (timeout? check $(SERIAL_LOG))"; \
		mcopy -o -i "$(DISK_IMG)" "$(DISK_EFI_DIR)/asdboot.conf" ::/EFI/BOOT/asdboot.conf 2>/dev/null || true; \
		mcopy -o -i "$(DISK_IMG)" "$(DISK_EFI_DIR)/asdboot.conf" ::/boot/asdboot.conf 2>/dev/null || true; \
		exit 1; \
	fi
	$(call write-asdboot-conf,"$(RUN_DIR)/asdboot.conf")
	@mcopy -i "$(DISK_IMG)" "$(RUN_DIR)/asdboot.conf" ::/EFI/BOOT/asdboot.conf
	@mcopy -i "$(DISK_IMG)" "$(RUN_DIR)/asdboot.conf" ::/boot/asdboot.conf

# Headless autotest for hxtest (file save regression).
bfd-hxtest: all
	$(call check-mtools)
	@mkdir -p "$(DEBUG_DIR)"
	@if [ ! -f "$(DISK_IMG)" ]; then $(MAKE) install; fi
	@if dd if="$(DISK_IMG)" bs=1 skip=512 count=8 2>/dev/null | grep -aq 'EFI PART'; then \
		echo "bfd-hxtest: $(DISK_IMG) is GPT layout; run: make install-fresh first."; \
		exit 1; \
	fi
	$(call write-asdboot-conf-hxtest,"$(RUN_DIR)/asdboot-hxtest.conf")
	@mcopy -i "$(DISK_IMG)" "$(RUN_DIR)/asdboot-hxtest.conf" ::/EFI/BOOT/asdboot.conf
	@mcopy -i "$(DISK_IMG)" "$(RUN_DIR)/asdboot-hxtest.conf" ::/boot/asdboot.conf
	@echo "Serial log: $(SERIAL_LOG)"
	@if [ ! -f "$(OVMF_CODE)" ] || [ ! -f "$(OVMF_VARS)" ]; then \
		echo "OVMF firmware not found."; exit 1; fi
	@if [ ! -f "$(OVMF_VARS_BFD)" ]; then cp "$(OVMF_VARS)" "$(OVMF_VARS_BFD)"; fi
	@rm -f "$(SERIAL_LOG)" "$(QEMU_DEBUG_LOG)"
	@echo "Running QEMU hxtest (~25s)..."
	@( command -v timeout >/dev/null && timeout 25 $(QEMU) \
		-machine q35,accel=tcg -cpu qemu64 -m 1024 -display none -no-reboot \
		-serial file:$(SERIAL_LOG) \
		-d guest_errors,int,cpu_reset -D $(QEMU_DEBUG_LOG) \
		-drive if=pflash,format=raw,readonly=on,file="$(OVMF_CODE)" \
		-drive if=pflash,format=raw,file="$(OVMF_VARS_BFD)" \
		-drive file="$(DISK_IMG)",if=none,id=inst0,format=raw \
		-device virtio-blk-pci,drive=inst0 \
	) || true
	@echo "--- serial.log (last 40 lines) ---"
	@tail -40 "$(SERIAL_LOG)" 2>/dev/null || true
	@if grep -q '\*\*\* EXCEPTION' "$(SERIAL_LOG)" 2>/dev/null; then \
		echo "FAIL: kernel exception"; \
	elif grep -q 'HXTEST OK' "$(SERIAL_LOG)" 2>/dev/null; then \
		echo "PASS: hxtest OK"; \
	elif grep -q 'HXTEST FAIL' "$(SERIAL_LOG)" 2>/dev/null; then \
		echo "FAIL: hxtest failed (see serial.log)"; \
		exit 1; \
	else \
		echo "WARN: no HXTEST result (timeout? check $(SERIAL_LOG))"; \
		exit 1; \
	fi
	$(call write-asdboot-conf,"$(RUN_DIR)/asdboot.conf")
	@mcopy -i "$(DISK_IMG)" "$(RUN_DIR)/asdboot.conf" ::/EFI/BOOT/asdboot.conf
	@mcopy -i "$(DISK_IMG)" "$(RUN_DIR)/asdboot.conf" ::/boot/asdboot.conf

# Network autotest — tests ping, DNS, TCP via nettest binary.
bfd-nettest: all
	$(call check-mtools)
	@mkdir -p "$(DEBUG_DIR)"
	@if [ ! -f "$(DISK_IMG)" ]; then $(MAKE) install; fi
	@if dd if="$(DISK_IMG)" bs=1 skip=512 count=8 2>/dev/null | grep -aq 'EFI PART'; then \
		echo "bfd-nettest: $(DISK_IMG) is GPT layout; run: make install-fresh first."; \
		exit 1; \
	fi
	$(call write-asdboot-conf-nettest,"$(RUN_DIR)/asdboot-nettest.conf")
	@mcopy -o -i "$(DISK_IMG)" "$(RUN_DIR)/asdboot-nettest.conf" ::/EFI/BOOT/asdboot.conf
	@mcopy -o -i "$(DISK_IMG)" "$(RUN_DIR)/asdboot-nettest.conf" ::/boot/asdboot.conf
	@if [ ! -f "$(OVMF_CODE)" ] || [ ! -f "$(OVMF_VARS)" ]; then \
		echo "OVMF firmware not found."; exit 1; fi
	@if [ ! -f "$(OVMF_VARS_BFD)" ]; then cp "$(OVMF_VARS)" "$(OVMF_VARS_BFD)"; fi
	@rm -f "$(SERIAL_LOG)" "$(QEMU_DEBUG_LOG)"
	@echo "Running QEMU nettest (timeout 45s)..."
	@( command -v timeout >/dev/null && timeout 45 $(QEMU) \
		-machine q35,accel=kvm:tcg -cpu qemu64 -m 1024 -display none -no-reboot \
		-serial file:$(SERIAL_LOG) \
		-d guest_errors -D $(QEMU_DEBUG_LOG) \
		-drive if=pflash,format=raw,readonly=on,file="$(OVMF_CODE)" \
		-drive if=pflash,format=raw,file="$(OVMF_VARS_BFD)" \
		-drive file="$(DISK_IMG)",if=none,id=inst0,format=raw \
		-device virtio-blk-pci,drive=inst0,bootindex=1 \
		-netdev user,id=net0,net=10.0.2.0/24,host=10.0.2.2,dns=10.0.2.3 \
		-device virtio-net-pci,netdev=net0 \
	) || true
	@echo "--- serial.log ---"
	@cat "$(SERIAL_LOG)" 2>/dev/null || echo "(no serial log)"
	@echo "--- end ---"
	@if grep -q 'NETTEST ALL OK' "$(SERIAL_LOG)" 2>/dev/null; then \
		echo "PASS: all network tests passed"; \
	elif grep -q 'NETTEST SOME FAILED' "$(SERIAL_LOG)" 2>/dev/null; then \
		echo "FAIL: some network tests failed (see log above)"; \
		exit 1; \
	else \
		echo "WARN: no NETTEST result (timeout or boot failure)"; \
		exit 1; \
	fi
	$(call write-asdboot-conf,"$(RUN_DIR)/asdboot.conf")
	@mcopy -i "$(DISK_IMG)" "$(RUN_DIR)/asdboot.conf" ::/EFI/BOOT/asdboot.conf
	@mcopy -i "$(DISK_IMG)" "$(RUN_DIR)/asdboot.conf" ::/boot/asdboot.conf

# -----------------------------------------------------------------------
# usb-image — bootable GPT disk image for writing to USB drives.
#
# Creates a raw disk image with a proper GPT + EFI System Partition (FAT32)
# so UEFI firmware on real hardware can find and boot OpenASD.
#
# Write to USB:
#   Linux:   sudo dd if=build/usb/openasd-1.0.img of=/dev/sdX bs=4M status=progress
#   Windows: use Rufus in "DD Image" mode
# -----------------------------------------------------------------------
usb-image: all
	@mkdir -p "$(USB_DIR)"
	@echo "==> Creating GPT disk image ($(USB_SIZE_MB) MiB)..."
	@dd if=/dev/zero of="$(USB_IMG)" bs=1M count=$(USB_SIZE_MB) 2>/dev/null
	@python3 scripts/make_gpt.py "$(USB_IMG)"
	@echo "==> Formatting ESP (FAT32 at 1 MiB offset)..."
	@mformat -i "$(USB_IMG)@@1M" -F -v "OPENASD" ::
	@mmd    -i "$(USB_IMG)@@1M" ::/EFI ::/EFI/BOOT ::/boot ::/bin ::/sbin
	@echo "==> Copying bootloader and kernel..."
	@mcopy  -o -i "$(USB_IMG)@@1M" "$(EFI_DIR)/BOOTX64.EFI"    ::/EFI/BOOT/BOOTX64.EFI
	@mcopy  -o -i "$(USB_IMG)@@1M" "$(EFI_DIR)/asdkernel.bin"  ::/EFI/BOOT/asdkernel.bin
	@mcopy  -o -i "$(USB_IMG)@@1M" "$(EFI_DIR)/asdboot.conf"   ::/EFI/BOOT/asdboot.conf
	@mcopy  -o -i "$(USB_IMG)@@1M" "$(BOOT_DIR)/asdkernel.bin" ::/boot/asdkernel.bin
	@mcopy  -o -i "$(USB_IMG)@@1M" "$(BOOT_DIR)/asdboot.conf"  ::/boot/asdboot.conf
	@echo "==> Copying userland binaries..."
	@mcopy  -o -i "$(USB_IMG)@@1M" userland/sh/build/asdsh     ::/bin/asdsh
	@for b in $(LIVE_BINS); do \
		mcopy -o -i "$(USB_IMG)@@1M" userland/bin/build/$$b ::/bin/$$b 2>/dev/null || true; \
	done
	@[ -f userland/hx/build/hx ] && mcopy -o -i "$(USB_IMG)@@1M" userland/hx/build/hx ::/bin/hx || true
	@mcopy  -o -i "$(USB_IMG)@@1M" userland/sbin/build/asdlog ::/sbin/asdlog 2>/dev/null || true
	@mcopy  -o -i "$(USB_IMG)@@1M" userland/sbin/build/netd   ::/sbin/netd   2>/dev/null || true
	@echo ""
	@echo "==> $(USB_IMG) ($$(du -h "$(USB_IMG)" | cut -f1))"
	@echo ""
	@echo "Write to USB (Linux):   sudo dd if=\"$(USB_IMG)\" of=/dev/sdX bs=4M status=progress"
	@echo "Write to USB (Windows): use Rufus → DD Image mode"

iso: all
	@mkdir -p "$(ISO_DIR)/EFI/BOOT" "$(ISO_DIR)/boot"
	@cp boot/asdboot.efi "$(ISO_DIR)/EFI/BOOT/BOOTX64.EFI"
	@cp kernel/build/asdkernel.bin "$(ISO_DIR)/EFI/BOOT/asdkernel.bin"
	@cp kernel/build/asdkernel.bin "$(ISO_DIR)/boot/asdkernel.bin"
	$(call write-asdboot-conf,"$(ISO_DIR)/EFI/BOOT/asdboot.conf")
	@cp "$(ISO_DIR)/EFI/BOOT/asdboot.conf" "$(ISO_DIR)/boot/asdboot.conf"
	@dd if=/dev/zero of="$(EFI_IMG)" bs=1k count=1440 2>/dev/null
	@mkfs.vfat -F 12 "$(EFI_IMG)" >/dev/null
	@mmd  -i "$(EFI_IMG)" ::/EFI ::/EFI/BOOT
	@mcopy -i "$(EFI_IMG)" "$(ISO_DIR)/EFI/BOOT/BOOTX64.EFI"   ::/EFI/BOOT/BOOTX64.EFI
	@mcopy -i "$(EFI_IMG)" "$(ISO_DIR)/EFI/BOOT/asdkernel.bin" ::/EFI/BOOT/asdkernel.bin
	@mcopy -i "$(EFI_IMG)" "$(ISO_DIR)/EFI/BOOT/asdboot.conf"  ::/EFI/BOOT/asdboot.conf
	@xorriso -as mkisofs \
		-o "$(ISO_IMG)" \
		-R -J -V "ASD" \
		-no-emul-boot \
		-eltorito-platform efi \
		-eltorito-boot EFI/efiboot.img \
		-isohybrid-gpt-basdat \
		"$(ISO_DIR)" 2>/dev/null
	@echo "==> $(ISO_IMG) ($$(du -sh "$(ISO_IMG)" | cut -f1))"

clean:
	$(MAKE) -C boot clean
	$(MAKE) -C kernel clean
	$(MAKE) -C userland clean
	@rm -rf "$(ISO_DIR)" "$(ISO_IMG)" "$(LIVE_IMG)" "$(BUILD_DIR)"
