CC = i686-w64-mingw32-gcc
BUILD = build

CFLAGS = -O2 -Wall
SMD_CFLAGS = -O2 -Wall -Wno-unused-function -Wno-unused-variable
SMD_DEFS =
LDFLAGS = -shared -static-libgcc -s -Wl,--kill-at

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

$(BUILD)/RefRain.o: src/RefRain.c src/d3dx9_font_compat.h | $(BUILD)
	$(CC) -c -o $@ $< $(CFLAGS)

# make refrain-debug : same, with logging (stage_select.log) enabled -> build/debug/d3d9.dll
refrain-debug: $(BUILD)/debug/d3d9.dll

$(BUILD)/debug/d3d9.dll: $(BUILD)/RefRain_debug.o | $(BUILD)/debug
	$(CC) -o $@ $^ $(LDFLAGS) -lwinmm

$(BUILD)/RefRain_debug.o: src/RefRain.c src/d3dx9_font_compat.h | $(BUILD)
	$(CC) -c -o $@ $< $(CFLAGS) -DREFRAIN_DEBUG_BUILD

# make smd : Smd sources -> build/ddraw.dll
smd: $(BUILD)/ddraw.dll

# make smd-debug : Debug Smd sources -> build/debug/ddraw.dll
smd-debug: $(BUILD)/debug/ddraw.dll

$(BUILD)/debug/ddraw.dll: $(BUILD)/Smd_debug.o $(BUILD)/Smd_d3d9_debug.o $(BUILD)/Smd_controller_debug.o | $(BUILD)/debug
	$(CC) -o $@ $^ $(LDFLAGS) -Wl,--allow-multiple-definition -lgdi32 -ldxguid

$(BUILD)/Smd_debug.o: src/Smd/Smd.c src/Smd/Smd_internal.h | $(BUILD)
	$(CC) -c -o $@ $< $(SMD_CFLAGS) -DSMD_DEBUG_BUILD $(SMD_DEFS)

$(BUILD)/Smd_d3d9_debug.o: src/Smd/Smd_d3d9.c src/Smd/Smd_internal.h | $(BUILD)
	$(CC) -c -o $@ $< $(SMD_CFLAGS) -DSMD_DEBUG_BUILD $(SMD_DEFS)

$(BUILD)/Smd_controller_debug.o: src/Smd/Smd_controller.c src/Smd/Smd_internal.h | $(BUILD)
	$(CC) -c -o $@ $< $(SMD_CFLAGS) -DSMD_DEBUG_BUILD $(SMD_DEFS)

$(BUILD)/ddraw.dll: $(BUILD)/Smd.o $(BUILD)/Smd_d3d9.o $(BUILD)/Smd_controller.o
	$(CC) -o $@ $^ $(LDFLAGS) -Wl,--allow-multiple-definition -lgdi32 -ldxguid

$(BUILD)/Smd.o: src/Smd/Smd.c src/Smd/Smd_internal.h | $(BUILD)
	$(CC) -c -o $@ $< $(SMD_CFLAGS) $(SMD_DEFS)

$(BUILD)/Smd_d3d9.o: src/Smd/Smd_d3d9.c src/Smd/Smd_internal.h | $(BUILD)
	$(CC) -c -o $@ $< $(SMD_CFLAGS) $(SMD_DEFS)

$(BUILD)/Smd_controller.o: src/Smd/Smd_controller.c src/Smd/Smd_internal.h | $(BUILD)
	$(CC) -c -o $@ $< $(SMD_CFLAGS) $(SMD_DEFS)

$(BUILD):
	mkdir -p $@

$(BUILD)/debug:
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all debug refrain refrain-debug smd smd-debug clean
