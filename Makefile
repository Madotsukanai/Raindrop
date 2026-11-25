.DEFAULT_GOAL := all
CC = i686-w64-mingw32-gcc
BUILD = build
VERSION := $(shell tr -d '\r\n' < VERSION)
VERSION_SOURCES := src/Smd/Smd.c src/RefRain.c
VERSION_STAMP := $(BUILD)/.version

# Keep the VERSION macro in the sources in sync with the VERSION file.
$(VERSION_STAMP): VERSION | $(BUILD)
	@for f in $(VERSION_SOURCES); do \
	  sed -i 's|^#define VERSION .*|#define VERSION "$(VERSION)"|' $$f; \
	done
	@printf '%s\n' '$(VERSION)' > $@
	@echo "Version synced: $(VERSION)"

CFLAGS = -O2 -Wall
SMD_CFLAGS = -O2 -Wall -Wno-unused-function -Wno-unused-variable
SMD_DEFS =
SMD_INCLUDES = -Irust/raindrop-gpu/include
RUST = cargo
RUST_TARGET_DIR = rust/raindrop-gpu/target/i686-pc-windows-gnu/release
RUSTLIB = $(RUST_TARGET_DIR)/libraindrop_gpu.a
RUST_RUNTIME_LIBS = -ladvapi32 -lws2_32 -lbcrypt -luserenv -lole32 -loleaut32 -lsynchronization -lntdll -lopengl32 -lgdi32 -lwindowsapp
RUST_D3D9 = rust/raindrop-d3d9/target/i686-pc-windows-gnu/release/raindrop_d3d9.dll
RUST_D3D9LIB = rust/raindrop-d3d9/target/i686-pc-windows-gnu/release/libraindrop_d3d9.a
RUST_DDRAWLIB = rust/raindrop-ddraw/target/i686-pc-windows-gnu/release/libraindrop_ddraw.a

$(RUST_DDRAWLIB): rust/raindrop-ddraw/Cargo.toml rust/raindrop-ddraw/src/*.rs $(RUST_D3D9LIB)
	cd rust/raindrop-ddraw && $(RUST) build --target i686-pc-windows-gnu --release --features wgpu-backend

$(RUST_D3D9): rust/raindrop-d3d9/Cargo.toml rust/raindrop-d3d9/src/*.rs $(RUSTLIB)
	cd rust/raindrop-d3d9 && $(RUST) build --target i686-pc-windows-gnu --release --features wgpu-backend

$(RUST_D3D9LIB): $(RUST_D3D9)

# make rust-d3d9 : Rust d3d9.dll (proxy default / wgpu backend via raindrop.ini)
rust-d3d9: $(BUILD)/rust/d3d9.dll

$(BUILD)/rust/d3d9.dll: $(RUST_D3D9) | $(BUILD)/rust
	cp $< $@
# -static keeps libwinpthread / libgcc from becoming DLL imports, so the shims
# stay single-file deployable now that the Rust kernels are linked in.
LDFLAGS = -shared -static -static-libgcc -s -Wl,--kill-at

all: refrain smd

# make debug : every debug build (logging enabled) -> build/debug/d3d9.dll and build/debug/ddraw.dll
debug: refrain-debug smd-debug

# make refrain : src/RefRain.c -> build/d3d9.dll
# Standalone deployment: place d3d9.dll beside RefRain.exe.  Windows loads
# it in preference to the system DLL, and it forwards Direct3DCreate9 to the
# real system d3d9.dll after installing this mod's hooks.
refrain: $(BUILD)/d3d9.dll

$(BUILD)/d3d9.dll: $(BUILD)/RefRain.o
	$(CC) -o $@ $^ $(LDFLAGS) -lwinmm

$(BUILD)/RefRain.o: src/RefRain.c src/d3dx9_font_compat.h src/RainUpdate.h $(VERSION_STAMP) | $(BUILD)
	$(CC) -c -o $@ src/RefRain.c $(CFLAGS)

# make refrain-debug : same, with logging (raindrop.log) enabled -> build/debug/d3d9.dll
refrain-debug: $(BUILD)/debug/d3d9.dll

$(BUILD)/debug/d3d9.dll: $(BUILD)/RefRain_debug.o | $(BUILD)/debug
	$(CC) -o $@ $^ $(LDFLAGS) -lwinmm

$(BUILD)/RefRain_debug.o: src/RefRain.c src/d3dx9_font_compat.h src/RainUpdate.h $(VERSION_STAMP) | $(BUILD)
	$(CC) -c -o $@ src/RefRain.c $(CFLAGS) -DREFRAIN_DEBUG_BUILD

# make smd : Smd sources -> build/ddraw.dll
smd: $(BUILD)/ddraw.dll

# make smd-debug : Debug Smd sources -> build/debug/ddraw.dll
smd-debug: $(BUILD)/debug/ddraw.dll

$(BUILD)/debug/ddraw.dll: $(BUILD)/Smd_debug.o $(BUILD)/Smd_d3d9_debug.o $(BUILD)/Smd_controller_debug.o $(RUSTLIB) $(RUST_DDRAWLIB) | $(BUILD)/debug
	$(CC) -o $@ $^ $(LDFLAGS) -Wl,--allow-multiple-definition -lgdi32 -ldxguid $(RUST_RUNTIME_LIBS)

$(BUILD)/Smd_debug.o: src/Smd/Smd.c src/Smd/Smd_internal.h src/RainUpdate.h $(VERSION_STAMP) | $(BUILD)
	$(CC) -c -o $@ src/Smd/Smd.c $(SMD_CFLAGS) -DSMD_DEBUG_BUILD $(SMD_DEFS) $(SMD_INCLUDES)

$(BUILD)/Smd_d3d9_debug.o: src/Smd/Smd_d3d9.c src/Smd/Smd_internal.h rust/raindrop-gpu/include/raindrop_gpu.h | $(BUILD)
	$(CC) -c -o $@ $< $(SMD_CFLAGS) -DSMD_DEBUG_BUILD $(SMD_DEFS) $(SMD_INCLUDES)

$(BUILD)/Smd_controller_debug.o: src/Smd/Smd_controller.c src/Smd/Smd_internal.h | $(BUILD)
	$(CC) -c -o $@ $< $(SMD_CFLAGS) -DSMD_DEBUG_BUILD $(SMD_DEFS) $(SMD_INCLUDES)

# Rust pixel kernels (raindrop-gpu crate). Release build is shared by debug DLLs too:
# the kernels carry no logging of their own and differ from a debug build only in speed.
$(RUSTLIB): rust/raindrop-gpu/Cargo.toml rust/raindrop-gpu/src/lib.rs rust/raindrop-gpu/src/kernels.rs
	cd rust/raindrop-gpu && $(RUST) build --target i686-pc-windows-gnu --release

$(BUILD)/ddraw.dll: $(BUILD)/Smd.o $(BUILD)/Smd_d3d9.o $(BUILD)/Smd_controller.o $(RUSTLIB) $(RUST_DDRAWLIB)
	$(CC) -o $@ $^ $(LDFLAGS) -Wl,--allow-multiple-definition -lgdi32 -ldxguid $(RUST_RUNTIME_LIBS)

$(BUILD)/Smd.o: src/Smd/Smd.c src/Smd/Smd_internal.h src/RainUpdate.h $(VERSION_STAMP) | $(BUILD)
	$(CC) -c -o $@ src/Smd/Smd.c $(SMD_CFLAGS) $(SMD_DEFS) $(SMD_INCLUDES)

$(BUILD)/Smd_d3d9.o: src/Smd/Smd_d3d9.c src/Smd/Smd_internal.h rust/raindrop-gpu/include/raindrop_gpu.h | $(BUILD)
	$(CC) -c -o $@ $< $(SMD_CFLAGS) $(SMD_DEFS) $(SMD_INCLUDES)

$(BUILD)/Smd_controller.o: src/Smd/Smd_controller.c src/Smd/Smd_internal.h | $(BUILD)
	$(CC) -c -o $@ $< $(SMD_CFLAGS) $(SMD_DEFS) $(SMD_INCLUDES)

$(BUILD):
	mkdir -p $@

$(BUILD)/rust:
	mkdir -p $@

$(BUILD)/debug:
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all debug refrain refrain-debug smd smd-debug rust-d3d9 clean
