# raindrop-ddraw

Rust replacement for 五月雨 (Samidare)'s DirectDraw7 + Direct3D7 emulation
layer (the `D7` emu in `src/Smd/Smd_d3d9.c`), running directly on the
`raindrop-d3d9` wgpu device — the D3D7 → d3d9 COM hop and the
`LoadLibrary("d3d9.dll")` indirection are gone.

## What it implements

- **IDirectDraw7** (30 slots), **IDirectDrawSurface7** (49 slots),
  **IDirect3D7** (8 slots), **IDirect3DDevice7** (49 slots) — slot indices
  verified against the C emu's vtables. QI accepts IUnknown / DD1–DD4 / DD7 /
  D3D7 / Dev7 like the C emu.
- Sysmem surfaces with masks/shifts (16/32 bpp), stretch+color-key software
  blits (C `D7_Blit` parity), Lock/Unlock with partial-dirty tracking.
- Texture upload straight into the d3d9 texture's system-memory level via the
  `raindrop-gpu` SIMD kernels (`upload_conv16` / `upload_xrgb32`), flushed once
  per frame by the renderer.
- `Blt`/`BltFast` onto the primary / 3D back buffer drawn as a textured quad
  into the wgpu scene (C `D7_BltToDev` parity, state save/restore included),
  followed by a single-swapchain present with device-lost recovery.
- `GetDC` syncs the last rendered frame back into the surface (readback via
  `GetRenderTargetData`) and hands out a GDI memory DC over a DIB section.
- `GetDeviceIdentifier` reports `Direct3D9 (Smd D3D7 emulation on <backend>)`
  `<backend>` is always "wgpu" (the only backend).
- Deferred-recording renderer state mirrors (rs/tss/sampler) so the C mod's
  batch vtable hooks (Dev7 slots 25/35) keep working: the vtables are leaked
  **mutable** memory and `SmdBatchFlushQuads` is invoked through a C hook
  before every state change.

## C-side integration (`src/Smd/Smd_d3d9.c`)

- The C shim keeps the `DirectDrawCreateEx` export and the mod patches.
- `raindrop.ini [Graphics] API=` selects the emulation layer:
  - `DirectDraw` — this crate (the DirectDraw emulation; wgpu is the only
    backend, there is no backend setting),
  - `D3D9` — the D3D9-based emulation (C D7 emu over the d3d9.dll in the
    game dir).
- C hooks passed to `raindrop_ddraw_create`: coop (hotkey subclass +
  borderless setup), win (borderless at CreateDevice), flush
  (`SmdBatchFlushQuads`). VSync interval is computed C-side
  (`D7E_ConfigVsync`).

## Build

```
make smd            # build/ddraw.dll (links libraindrop_ddraw.a + libraindrop_gpu.a)
```

Requires the `wgpu-backend` feature (enabled by the Makefile rule); all
32-bit (`i686-pc-windows-gnu`), Vulkan/GL backends, falls back gracefully
(`DDERR_GENERIC`) when wgpu is unavailable so the caller can fall back to the
C emu or system ddraw.

## Known differences from the C emu

- Present fills the whole swapchain; the C emu's aspect-fit letterbox
  (`D7_PresentFS`) is not replicated.
- No z-buffer/depth (same as the C emu), palettes/clipper unsupported (same).
- XYZ→XYZRHW via SetTransform matrices is not needed by 五月雨 (XYZRHW-only)
  and is not implemented.
