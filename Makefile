CC = i686-w64-mingw32-gcc
BUILD = build

CFLAGS = -O2 -Wall
SMD_CFLAGS = -O2 -Wall -Wno-unused-function -Wno-unused-variable
SMD_DEFS =
LDFLAGS = -shared -static-libgcc -s -Wl,--kill-at

all: refrain smd

# make refrain : RefRain.c -> build/d3d9.dll
# Standalone deployment: place d3d9.dll beside RefRain.exe.  Windows loads
# it in preference to the system DLL, and it forwards Direct3DCreate9 to the
# real system d3d9.dll after installing this mod's hooks.
refrain: $(BUILD)/d3d9.dll

$(BUILD)/d3d9.dll: $(BUILD)/RefRain.o
	$(CC) -o $@ $^ $(LDFLAGS) -lwinmm

$(BUILD)/RefRain.o: RefRain.c | $(BUILD)
	$(CC) -c -o $@ $< $(CFLAGS)

# make smd : Smd.c -> build/ddraw.dll
smd: $(BUILD)/ddraw.dll

$(BUILD)/ddraw.dll: $(BUILD)/Smd.o
	$(CC) -o $@ $^ $(LDFLAGS) -Wl,--allow-multiple-definition

$(BUILD)/Smd.o: Smd.c | $(BUILD)
	$(CC) -c -o $@ $< $(SMD_CFLAGS) $(SMD_DEFS)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all refrain smd clean
