CC = i686-w64-mingw32-gcc
CFLAGS = -O2 -Wall -static-libgcc -lwinmm

# Standalone deployment: place d3d9.dll beside RefRain.exe.  Windows loads
# it in preference to the system DLL, and it forwards Direct3DCreate9 to the
# real system d3d9.dll after installing this mod's hooks.
all: d3d9.dll

d3d9.dll: refrain_stage_select.c
	$(CC) -shared -o $@ $< $(CFLAGS) -s -Wl,--kill-at

clean:
	rm -f d3d9.dll

.PHONY: all clean
