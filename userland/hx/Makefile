# SPDX-License-Identifier: MIT
# hx — Helix-inspired modal editor (Rust, no_std, Mach-O)

RUST_TARGET = x86_64-apple-darwin
LIBASD      = ../libasd/build/libasd.a
LIBASD_SYS  = ../libasd/rust/asd_sys/target/$(RUST_TARGET)/release/libasd_sys.a
START_OBJ   = ../libasd/build/start.o
LD          = ld64.lld
LDFLAGS     = -arch x86_64 \
              -platform_version macos 13.0 13.0 \
              -e _start

# Locate cargo/rustup: try known Windows path first, fall back to $HOME (Linux/Mac)
_CARGO_SEARCH := /c/Users/Администратор/.cargo/bin/cargo \
                 /c/Users/Administrator/.cargo/bin/cargo \
                 $(HOME)/.cargo/bin/cargo
CARGO  := $(firstword $(wildcard $(_CARGO_SEARCH)))
RUSTUP := $(patsubst %cargo,%rustup,$(CARGO))

BUILD  = build
TARGET = $(BUILD)/hx

.PHONY: all clean libasd_sys _build_hx

all:
	@if [ -n "$(CARGO)" ] && [ -f "$(CARGO)" ]; then \
		$(MAKE) _build_hx || echo "  [skip] Failed to build hx editor (common on Windows without ld64.lld)"; \
	else \
		echo "  [skip] Rust/cargo not found — skipping hx editor (install rustup to enable)"; \
	fi

_build_hx: libasd_sys $(TARGET)

libasd_sys:
	$(MAKE) -C ../libasd/rust

$(BUILD):
	mkdir -p $(BUILD)

$(TARGET): src/lib.rs src/editor.rs src/buf.rs src/render.rs src/syscall.rs src/panic_handler.rs Cargo.toml | $(BUILD)
	$(RUSTUP) target add $(RUST_TARGET) 2>/dev/null || true
	PATH="$$(dirname $(CARGO)):$$PATH" $(CARGO) build --target $(RUST_TARGET) --release
	$(LD) $(LDFLAGS) \
		$(START_OBJ) \
		target/$(RUST_TARGET)/release/libhx.a \
		$(LIBASD_SYS) \
		$(LIBASD) \
		-o $(TARGET)

clean:
	$(CARGO) clean
	rm -rf $(BUILD)
