// Raindrop GPU - Rust side of the Raindrop shim DLLs.
//
// Currently provides:
//   * `kernels`   - fearless_simd accelerated pixel conversion kernels (semantics of
//                   D7_ToArgb / upload / color key in src/Smd/Smd_d3d9.c)
//   * C-ABI exports consumed by the C shim DLLs (see include/raindrop_gpu.h)
//   * `gpu`       - wgpu backend skeleton behind the `wgpu-backend` feature
//
//! C-ABI (all functions are cdecl, `extern "C"`, non-stdcall):
#![deny(unsafe_op_in_unsafe_fn)]

pub mod kernels;

#[cfg(feature = "wgpu-backend")]
pub mod gpu;

use kernels::{Fmt16, KEY_FORCE_TRANSPARENT};

/// Handle type used as the `fmt` code in the C ABI.
pub const FMT_565: i32 = 0;
pub const FMT_555: i32 = 1;
pub const FMT_5551: i32 = 2;
pub const FMT_4444: i32 = 3;

/// Convert a 16bpp source region to ARGB8888 (dst is w u32 elements per row).
///
/// # Safety
/// `src`/`dst` must be valid for `h` rows of `w` elements with the given pitches
/// (pitches are in elements, not bytes).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn raindrop_upload_conv16(
    dst: *mut u32,
    dst_pitch: usize,
    src: *const u16,
    src_pitch: usize,
    w: usize,
    h: usize,
    fmt: i32,
    has_key: i32,
    key: u16,
    key_cmp_mask: u16,
) {
    unsafe {
        if dst.is_null() || src.is_null() || w == 0 || h == 0 {
            return;
        }
        let Some(fmt) = Fmt16::from_code(fmt) else { return };
        let key = if has_key != 0 { Some((key, key_cmp_mask)) } else { None };
        for y in 0..h {
            let s = std::slice::from_raw_parts(src.add(y * src_pitch), w);
            let d = std::slice::from_raw_parts_mut(dst.add(y * dst_pitch), w);
            kernels::upload_conv16(d, s, fmt, key);
        }
    }
}

/// Upload 32bpp rows (X8R8G8B8 / A8R8G8B8 semantics of the fast path in D7_Tex).
///
/// # Safety
/// Same constraints as `raindrop_upload_conv16`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn raindrop_upload_xrgb32(
    dst: *mut u32,
    dst_pitch: usize,
    src: *const u32,
    src_pitch: usize,
    w: usize,
    h: usize,
    add_a: u32,        // 0 when format has alpha bits, else 0xFF000000
    has_key: i32,
    key: u32,          // pre-masked key (key & ~alpha_mask)
    key_cmp_mask: u32, // ~alpha_mask
) {
    unsafe {
        if dst.is_null() || src.is_null() || w == 0 || h == 0 {
            return;
        }
        let key = if has_key != 0 { Some((key, key_cmp_mask)) } else { None };
        for y in 0..h {
            let s = std::slice::from_raw_parts(src.add(y * src_pitch), w);
            let d = std::slice::from_raw_parts_mut(dst.add(y * dst_pitch), w);
            kernels::upload_xrgb32(d, s, add_a, key);
        }
    }
}

/// Fill a rectangular region with an ARGB8888 value (dst rows are u32 elements).
///
/// # Safety
/// `dst` must be valid for `h` rows of at least `x + w` elements with `dst_pitch` per row.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn raindrop_fill_u32(
    dst: *mut u32,
    dst_pitch: usize,
    x: usize,
    y: usize,
    w: usize,
    h: usize,
    color: u32,
) {
    unsafe {
        let _ = y; // fill operates on the given row base
        if dst.is_null() || w == 0 || h == 0 {
            return;
        }
        for r in 0..h {
            let d = std::slice::from_raw_parts_mut(dst.add(r * dst_pitch + x), w);
            d.fill(color);
        }
    }
}

/// Convenience export for the C side to probe the crate at load time.
#[unsafe(no_mangle)]
pub extern "C" fn raindrop_gpu_abi_version() -> u32 {
    1
}

/// Used by C to check a color key result constant if needed.
#[unsafe(no_mangle)]
pub extern "C" fn raindrop_key_transparent_mask() -> u32 {
    KEY_FORCE_TRANSPARENT
}
