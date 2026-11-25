/*
 * raindrop_gpu.h - C interface to the Rust raindrop-gpu crate.
 *
 * Build (from rust/raindrop-gpu):
 *   Linux cross:   cargo build --target i686-pc-windows-gnu --release [--features wgpu-backend]
 *   Windows MSVC:  cargo build --target i686-pc-windows-msvc --release [--features wgpu-backend]
 *
 * Link with target/<triple>/release/raindrop_gpu.dll (dynamic) or
 * libraindrop_gpu.a (static; then also link the Rust runtime libs shown by cargo).
 * All functions are cdecl (plain extern "C"); no stdcall decoration.
 */
#ifndef RAINDROP_GPU_H
#define RAINDROP_GPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ABI probe. Returns 1 for the current crate. */
uint32_t raindrop_gpu_abi_version(void);

/* Value to AND an ARGB pixel with to force alpha 0 (color-key transparency). */
uint32_t raindrop_key_transparent_mask(void);

/* ---- pixel kernels (no wgpu dependency) ----------------------------------- */

/* 16bpp source layouts. */
enum {
    RAINDROP_FMT_565  = 0, /* R 0xF800 / G 0x07E0 / B 0x001F                */
    RAINDROP_FMT_555  = 1, /* R 0x7C00 / G 0x03E0 / B 0x001F                */
    RAINDROP_FMT_5551 = 2, /* R 0x7C00 / G 0x03E0 / B 0x001F / A 0x8000     */
    RAINDROP_FMT_4444 = 3  /* R 0x0F00 / G 0x00F0 / B 0x000F / A 0xF000     */
};

/*
 * Convert 16bpp source rows to ARGB8888 destination rows.
 * Pitches are in ELEMENTS (u16 for src, u32 for dst), matching the D7Surf fields.
 * Colour key: pixel is keyed when (raw & key_cmp_mask) == key; keyed output pixels
 * get alpha 0 (same as the D7_Tex upload path). has_key == 0 disables keying.
 */
void raindrop_upload_conv16(
    uint32_t *dst, size_t dst_pitch,      /* ARGB8888 out, u32 per pixel        */
    const uint16_t *src, size_t src_pitch,/* raw 16bpp in                       */
    size_t w, size_t h,
    int fmt,                              /* RAINDROP_FMT_*                     */
    int has_key, uint16_t key, uint16_t key_cmp_mask);

/*
 * 32bpp upload (X8R8G8B8 / A8R8G8B8 fast path of D7_Tex):
 *   out = raw | add_a ; keyed pixels (per key_cmp_mask) get alpha 0.
 * Pass add_a = 0x00FF0000 for alpha-less formats, 0 when A8R8G8B8.
 * key / key_cmp_mask must be pre-masked ((key & ~alpha_mask), ~alpha_mask).
 */
void raindrop_upload_xrgb32(
    uint32_t *dst, size_t dst_pitch,
    const uint32_t *src, size_t src_pitch,
    size_t w, size_t h,
    uint32_t add_a,
    int has_key, uint32_t key, uint32_t key_cmp_mask);

/* Fill a rectangle with an ARGB8888 value. */
void raindrop_fill_u32(
    uint32_t *dst, size_t dst_pitch,
    size_t x, size_t y, size_t w, size_t h, uint32_t color);

/* ---- wgpu backend (build with --features wgpu-backend) -------------------- */

typedef struct RgContext RgContext;
typedef struct RgTexture RgTexture;

/* Returns NULL when no Vulkan/GL adapter is available (fall back to legacy path). */
RgContext *raindrop_gpu_init(void);
void raindrop_gpu_destroy(RgContext *ctx);

/* BGRA8 (A8R8G8B8) 2D texture with TEXTURE_BINDING|COPY_DST usage. */
RgTexture *raindrop_texture_create(RgContext *ctx, uint32_t w, uint32_t h);
void raindrop_texture_destroy(RgTexture *tex);

/*
 * Upload converted ARGB8888 rows into the texture at (x, y).
 * data holds h rows of pitch_pixels u32 pixels (dst_pitch semantics as above).
 */
void raindrop_texture_upload(
    RgContext *ctx, RgTexture *tex,
    uint32_t x, uint32_t y, uint32_t w, uint32_t h,
    const void *data, uint32_t pitch_pixels);

/* Flush pending work; call once per frame after uploads. */
void raindrop_gpu_poll(RgContext *ctx);

#ifdef __cplusplus
}
#endif
#endif /* RAINDROP_GPU_H */
