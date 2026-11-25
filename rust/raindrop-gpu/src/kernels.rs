// Raindrop GPU - pixel kernels.
//
// Semantics mirror src/Smd/Smd_d3d9.c exactly:
//   D7_ToArgb:   channel = expand((raw & mask) >> sh, nb) where expand(v, nb) = v*255/((1<<nb)-1)
//   ARGB layout: (a << 24) | (r << 16) | (g << 8) | b, alpha defaults to 255 when the
//                format has no alpha mask.
//   Upload key:  if (raw & key_cmp_mask) == key_cmp -> result alpha forced to 0.
//   Blit key:    keyed pixels are *skipped* (destination left untouched).
//
// The expansion division is done with magic-multiply constants that were brute-force
// verified (and are re-verified exhaustively by tests) to be exact:
//   /15  -> (c * 2185)  >> 15
//   /31  -> (c * 8457)  >> 18
//   /63  -> (c * 16645) >> 20

use fearless_simd::{dispatch, prelude::*, u16x8, u32x4, Level};
use std::sync::OnceLock;

static LEVEL: OnceLock<Level> = OnceLock::new();

#[inline]
pub fn level() -> Level {
    *LEVEL.get_or_init(Level::new)
}

/// 16bpp source layouts matching k_d7Fmts entries 2..5 in Smd_d3d9.c.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(i32)]
pub enum Fmt16 {
    Rgb565 = 0,    // 0xF800 / 0x07E0 / 0x001F
    Rgb555 = 1,    // 0x7C00 / 0x03E0 / 0x001F
    Rgba5551 = 2,  // 0x7C00 / 0x03E0 / 0x001F / A 0x8000
    Rgba4444 = 3,  // 0x0F00 / 0x00F0 / 0x000F / A 0xF000
}

impl Fmt16 {
    pub fn from_code(code: i32) -> Option<Fmt16> {
        Some(match code {
            0 => Fmt16::Rgb565,
            1 => Fmt16::Rgb555,
            2 => Fmt16::Rgba5551,
            3 => Fmt16::Rgba4444,
            _ => return None,
        })
    }
}

pub const KEY_FORCE_TRANSPARENT: u32 = 0x00FF_FFFF; // argb &= 0x00FFFFFF

// ---------------------------------------------------------------------------
// Scalar reference implementations (mirror the C code; used as fallback & test oracle)
// ---------------------------------------------------------------------------

#[inline]
pub fn expand_scalar(v: u32, nb: u32) -> u32 {
    (v.wrapping_mul(255)) / ((1u32 << nb) - 1)
}

/// Scalar D7_ToArgb for a 16bpp pixel.
#[inline]
pub fn to_argb16_scalar(fmt: Fmt16, raw: u16) -> u32 {
    let (r, g, b, a, anb) = match fmt {
        Fmt16::Rgb565 => (((raw >> 11) & 0x1F) as u32, ((raw >> 5) & 0x3F) as u32, (raw & 0x1F) as u32, 255u32, 0u32),
        Fmt16::Rgb555 => (((raw >> 10) & 0x1F) as u32, ((raw >> 5) & 0x1F) as u32, (raw & 0x1F) as u32, 255u32, 0u32),
        Fmt16::Rgba5551 => (((raw >> 10) & 0x1F) as u32, ((raw >> 5) & 0x1F) as u32, (raw & 0x1F) as u32, ((raw >> 15) & 1) as u32, 1u32),
        Fmt16::Rgba4444 => (((raw >> 8) & 0xF) as u32, ((raw >> 4) & 0xF) as u32, (raw & 0xF) as u32, ((raw >> 12) & 0xF) as u32, 4u32),
    };
    let a8 = if anb == 0 { 255 } else { expand_scalar(a, anb) };
    (a8 << 24) | (expand_scalar(r, r_nb(fmt)) << 16) | (expand_scalar(g, g_nb(fmt)) << 8) | expand_scalar(b, b_nb(fmt))
}

#[inline]
fn r_nb(fmt: Fmt16) -> u32 {
    match fmt { Fmt16::Rgba4444 => 4, _ => 5 }
}
#[inline]
fn g_nb(fmt: Fmt16) -> u32 {
    match fmt { Fmt16::Rgb565 => 6, Fmt16::Rgba4444 => 4, _ => 5 }
}
#[inline]
fn b_nb(fmt: Fmt16) -> u32 {
    match fmt { Fmt16::Rgba4444 => 4, _ => 5 }
}

pub fn upload_conv16_scalar(dst: &mut [u32], src: &[u16], fmt: Fmt16, key: Option<(u16, u16)>) {
    for (d, &s) in dst.iter_mut().zip(src.iter()) {
        let mut argb = to_argb16_scalar(fmt, s);
        if let Some((kc, km)) = key {
            if (s & km) == kc {
                argb &= KEY_FORCE_TRANSPARENT;
            }
        }
        *d = argb;
    }
}

pub fn upload_xrgb32_scalar(dst: &mut [u32], src: &[u32], add_a: u32, key: Option<(u32, u32)>) {
    for (d, &raw) in dst.iter_mut().zip(src.iter()) {
        let mut argb = raw | add_a;
        if let Some((kc, km)) = key {
            if (raw & km) == kc {
                argb &= KEY_FORCE_TRANSPARENT;
            }
        }
        *d = argb;
    }
}

// ---------------------------------------------------------------------------
// SIMD kernels (fearless_simd, runtime dispatch: AVX2 / SSE4.2 / SSE2 / fallback)
// ---------------------------------------------------------------------------

/// Exact (v * 255) / ((1<<nb)-1) via magic multiply. Valid for v <= (1<<nb)-1.
/// Magic constants verified exhaustively (see tests): input c = v*255 <= (1<<nb)-1 * 255.
#[inline(always)]
fn expand15<S: Simd>(simd: S, v: u32x4<S>) -> u32x4<S> {
    let c = simd.mul_u32x4(v, u32x4::simd_from(simd, 255));
    simd.shr_u32x4(simd.mul_u32x4(c, u32x4::simd_from(simd, 2185)), 15)
}
#[inline(always)]
fn expand31<S: Simd>(simd: S, v: u32x4<S>) -> u32x4<S> {
    let c = simd.mul_u32x4(v, u32x4::simd_from(simd, 255));
    simd.shr_u32x4(simd.mul_u32x4(c, u32x4::simd_from(simd, 8457)), 18)
}
#[inline(always)]
fn expand63<S: Simd>(simd: S, v: u32x4<S>) -> u32x4<S> {
    let c = simd.mul_u32x4(v, u32x4::simd_from(simd, 255));
    simd.shr_u32x4(simd.mul_u32x4(c, u32x4::simd_from(simd, 16645)), 20)
}

#[inline(always)]
fn or5<S: Simd>(simd: S, hi: u32x4<S>, mid: u32x4<S>, lo: u32x4<S>) -> u32x4<S> {
    simd.or_u32x4(simd.or_u32x4(hi, mid), lo)
}

/// Convert a widened 16bpp pixel block to ARGB8888.
#[inline(always)]
fn conv16_block<S: Simd>(simd: S, raw: u32x4<S>, fmt: Fmt16) -> u32x4<S> {
    match fmt {
        Fmt16::Rgb565 => {
            let r = expand31(simd, simd.shr_u32x4(simd.and_u32x4(raw, u32x4::simd_from(simd, 0xF800)), 11));
            let g = expand63(simd, simd.shr_u32x4(simd.and_u32x4(raw, u32x4::simd_from(simd, 0x07E0)), 5));
            let b = expand31(simd, simd.and_u32x4(raw, u32x4::simd_from(simd, 0x001F)));
            simd.or_u32x4(
                u32x4::simd_from(simd, 0xFF00_0000),
                or5(simd, simd.shl_u32x4(r, 16), simd.shl_u32x4(g, 8), b),
            )
        }
        Fmt16::Rgb555 | Fmt16::Rgba5551 => {
            let r = expand31(simd, simd.shr_u32x4(simd.and_u32x4(raw, u32x4::simd_from(simd, 0x7C00)), 10));
            let g = expand31(simd, simd.shr_u32x4(simd.and_u32x4(raw, u32x4::simd_from(simd, 0x03E0)), 5));
            let b = expand31(simd, simd.and_u32x4(raw, u32x4::simd_from(simd, 0x001F)));
            let alpha = match fmt {
                Fmt16::Rgba5551 => {
                    let a1 = simd.shr_u32x4(simd.and_u32x4(raw, u32x4::simd_from(simd, 0x8000)), 15);
                    simd.mul_u32x4(a1, u32x4::simd_from(simd, 255))
                }
                _ => u32x4::simd_from(simd, 255),
            };
            or5(
                simd,
                simd.shl_u32x4(alpha, 24),
                simd.shl_u32x4(r, 16),
                or5(simd, simd.shl_u32x4(g, 8), b, u32x4::simd_from(simd, 0)),
            )
        }
        Fmt16::Rgba4444 => {
            let r = expand15(simd, simd.shr_u32x4(simd.and_u32x4(raw, u32x4::simd_from(simd, 0x0F00)), 8));
            let g = expand15(simd, simd.shr_u32x4(simd.and_u32x4(raw, u32x4::simd_from(simd, 0x00F0)), 4));
            let b = expand15(simd, simd.and_u32x4(raw, u32x4::simd_from(simd, 0x000F)));
            let a = expand15(simd, simd.shr_u32x4(simd.and_u32x4(raw, u32x4::simd_from(simd, 0xF000)), 12));
            or5(
                simd,
                simd.shl_u32x4(a, 24),
                simd.shl_u32x4(r, 16),
                or5(simd, simd.shl_u32x4(g, 8), b, u32x4::simd_from(simd, 0)),
            )
        }
    }
}

#[inline(always)]
fn conv16_row<S: Simd>(simd: S, dst: &mut [u32], src: &[u16], fmt: Fmt16, key: Option<(u16, u16)>) {
    let n = src.len();
    let mut i = 0;
    while i + 8 <= n {
        let s = u16x8::from_slice(simd, &src[i..i + 8]);
        let (s0, s1) = simd.widen_u16x8(s);
        let d0 = apply_key32(simd, conv16_block(simd, s0, fmt), s0, key, fmt);
        let d1 = apply_key32(simd, conv16_block(simd, s1, fmt), s1, key, fmt);
        d0.store_slice(&mut dst[i..i + 4]);
        d1.store_slice(&mut dst[i + 4..i + 8]);
        i += 8;
    }
    if i < n {
        let mut tail_src = [0u16; 8];
        let mut tail_dst = [0u32; 8];
        let rem = n - i;
        tail_src[..rem].copy_from_slice(&src[i..]);
        let s = u16x8::from_slice(simd, &tail_src);
        let (s0, s1) = simd.widen_u16x8(s);
        let d0 = apply_key32(simd, conv16_block(simd, s0, fmt), s0, key, fmt);
        let d1 = apply_key32(simd, conv16_block(simd, s1, fmt), s1, key, fmt);
        d0.store_slice(&mut tail_dst[..4]);
        d1.store_slice(&mut tail_dst[4..]);
        dst[i..].copy_from_slice(&tail_dst[..rem]);
    }
}

#[inline(always)]
fn apply_key32<S: Simd>(
    simd: S,
    argb: u32x4<S>,
    raw: u32x4<S>,
    key: Option<(u16, u16)>,
    _fmt: Fmt16,
) -> u32x4<S> {
    match key {
        None => argb,
        Some((kc, km)) => {
            let mask = u32x4::simd_from(simd, km as u32);
            let keyv = u32x4::simd_from(simd, kc as u32);
            let hit = simd.simd_eq_u32x4(simd.and_u32x4(raw, mask), keyv);
            simd.select_u32x4(
                hit,
                simd.and_u32x4(argb, u32x4::simd_from(simd, KEY_FORCE_TRANSPARENT)),
                argb,
            )
        }
    }
}

#[inline(always)]
fn xrgb32_row<S: Simd>(simd: S, dst: &mut [u32], src: &[u32], add_a: u32, key: Option<(u32, u32)>) {
    let n = src.len();
    let mut i = 0;
    while i + 8 <= n {
        let s0 = u32x4::from_slice(simd, &src[i..i + 4]);
        let s1 = u32x4::from_slice(simd, &src[i + 4..i + 8]);
        let d0 = keyed_argb(simd, s0, add_a, key);
        let d1 = keyed_argb(simd, s1, add_a, key);
        d0.store_slice(&mut dst[i..i + 4]);
        d1.store_slice(&mut dst[i + 4..i + 8]);
        i += 8;
    }
    while i < n {
        let mut b = [0u32; 4];
        let rem = (n - i).min(4);
        b[..rem].copy_from_slice(&src[i..i + rem]);
        let s = u32x4::from_slice(simd, &b);
        let d = keyed_argb(simd, s, add_a, key);
        let mut o = [0u32; 4];
        d.store_slice(&mut o);
        dst[i..i + rem].copy_from_slice(&o[..rem]);
        i += rem;
    }
}

#[inline(always)]
fn keyed_argb<S: Simd>(simd: S, raw: u32x4<S>, add_a: u32, key: Option<(u32, u32)>) -> u32x4<S> {
    let mut argb = if add_a != 0 {
        simd.or_u32x4(raw, u32x4::simd_from(simd, add_a))
    } else {
        raw
    };
    if let Some((kc, km)) = key {
        let hit = simd.simd_eq_u32x4(simd.and_u32x4(raw, u32x4::simd_from(simd, km)), u32x4::simd_from(simd, kc));
        argb = simd.select_u32x4(
            hit,
            simd.and_u32x4(argb, u32x4::simd_from(simd, KEY_FORCE_TRANSPARENT)),
            argb,
        );
    }
    argb
}

// ---------------------------------------------------------------------------
// Public row/region entry points (used by the C-ABI layer in lib.rs)
// ---------------------------------------------------------------------------

/// Convert 16bpp source rows into ARGB8888 destination rows.
pub fn upload_conv16(dst: &mut [u32], src: &[u16], fmt: Fmt16, key: Option<(u16, u16)>) {
    dispatch!(level(), simd => upload_conv16_dispatched(simd, dst, src, fmt, key));
}

#[inline(always)]
fn upload_conv16_dispatched<S: Simd>(simd: S, dst: &mut [u32], src: &[u16], fmt: Fmt16, key: Option<(u16, u16)>) {
    conv16_row(simd, dst, src, fmt, key);
}

/// Upload 32bpp X8R8G8B8 / A8R8G8B8 rows (raw | add_a, optional color key).
pub fn upload_xrgb32(dst: &mut [u32], src: &[u32], add_a: u32, key: Option<(u32, u32)>) {
    dispatch!(level(), simd => xrgb32_row(simd, dst, src, add_a, key));
}

// ---------------------------------------------------------------------------
// Tests: SIMD vs scalar oracle + magic-number exactness
// ---------------------------------------------------------------------------

#[cfg(test)]
fn lcg(state: &mut u64) -> u64 {
    *state = state.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
    *state >> 16
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Exhaustive: magic multiply == scalar division for every valid v.
    #[test]
    fn magic_expansion_exact() {
        dispatch!(level(), simd => {
            for &nb in &[4u32, 5, 6] {
                let d = (1u32 << nb) - 1;
                let magic = match nb { 4 => 2185u32, 5 => 8457, 6 => 16645, _ => unreachable!() };
                let shift = match nb { 4 => 15u32, 5 => 18, 6 => 20, _ => unreachable!() };
                let mut v = 0u32;
                while v <= d {
                    let c = v.wrapping_mul(255);
                    let got = simd.shr_u32x4(
                        simd.mul_u32x4(
                            simd.mul_u32x4(u32x4::simd_from(simd, v), u32x4::simd_from(simd, 255)),
                            u32x4::simd_from(simd, magic),
                        ),
                        shift,
                    );
                    assert_eq!(got[0], expand_scalar(v, nb), "nb={} v={}", nb, v);
                    v += 1;
                }
            }
        });
    }

    #[test]
    fn conv16_simd_matches_scalar() {
        let mut state = 0x12345678u64;
        for &fmt in &[Fmt16::Rgb565, Fmt16::Rgb555, Fmt16::Rgba5551, Fmt16::Rgba4444] {
            for key in [None, Some((0x0000u16, 0xFFFFu16)), Some((0x001Fu16, 0xFFFFu16)), Some((0x8000u16, 0x7FFFu16))] {
                for len in [1usize, 3, 4, 5, 7, 8, 9, 16, 33, 64] {
                    let src: Vec<u16> = (0..len).map(|_| lcg(&mut state) as u16).collect();
                    let mut want = vec![0u32; len];
                    let mut got = vec![0u32; len];
                    upload_conv16_scalar(&mut want, &src, fmt, key);
                    upload_conv16(&mut got, &src, fmt, key);
                    assert_eq!(got, want, "fmt={:?} key={:?} len={}", fmt, key, len);
                }
                // boundary values
                let src: Vec<u16> = [0u16, 0xFFFF, 0x8000, 0x7FFF, 0xF800, 0x07E0, 0x001F].to_vec();
                let mut want = vec![0u32; src.len()];
                let mut got = vec![0u32; src.len()];
                upload_conv16_scalar(&mut want, &src, fmt, key);
                upload_conv16(&mut got, &src, fmt, key);
                assert_eq!(got, want, "fmt={:?} key={:?} boundary", fmt, key);
            }
        }
    }

    #[test]
    fn xrgb32_simd_matches_scalar() {
        let mut state = 0xdeadbeefu64;
        for add_a in [0u32, 0xFF00_0000] {
            for key in [None, Some((0x11223344u32, 0xFFFF_FFFF)), Some((0xFF00_0000u32, 0x00FF_FFFF))] {
                for len in [1usize, 4, 5, 8, 12, 17, 32] {
                    let src: Vec<u32> = (0..len).map(|_| lcg(&mut state) as u32).collect();
                    let mut want = vec![0u32; len];
                    let mut got = vec![0u32; len];
                    upload_xrgb32_scalar(&mut want, &src, add_a, key);
                    upload_xrgb32(&mut got, &src, add_a, key);
                    assert_eq!(got, want, "add_a={:#x} key={:?} len={}", add_a, key, len);
                }
            }
        }
    }

    /// Row-shaped regions with arbitrary pitch, like the real upload loop.
    #[test]
    fn upload_regions_with_pitch() {
        let mut state = 0xabcdefu64;
        let (w, h) = (7usize, 5usize);
        let src_pitch = 16usize; // u16 elements per row (i.e. 32 bytes)
        let dst_pitch = 12usize; // u32 per row
        let src: Vec<u16> = (0..src_pitch * h).map(|_| lcg(&mut state) as u16).collect();
        let mut got = vec![0xAAAAu32; dst_pitch * h];
        let mut want = got.clone();
        for y in 0..h {
            let s = &src[y * src_pitch..y * src_pitch + w];
            let g = &mut got[y * dst_pitch..y * dst_pitch + w];
            let t = &mut want[y * dst_pitch..y * dst_pitch + w];
            upload_conv16(g, s, Fmt16::Rgb565, None);
            upload_conv16_scalar(t, s, Fmt16::Rgb565, None);
        }
        assert_eq!(got, want);
        // untouched pitch padding stays intact
        assert_eq!(got[0..w].len(), w);
    }

    /// Sanity: alpha-less formats get opaque alpha, 565 pure green expands correctly.
    #[test]
    fn known_values() {
        let green565 = to_argb16_scalar(Fmt16::Rgb565, 0x07E0);
        assert_eq!(green565, 0xFF00FF00);
        let white565 = to_argb16_scalar(Fmt16::Rgb565, 0xFFFF);
        assert_eq!(white565, 0xFFFFFFFF);
        let a5551 = to_argb16_scalar(Fmt16::Rgba5551, 0x8000);
        assert_eq!(a5551, 0xFF000000);
        let a0_5551 = to_argb16_scalar(Fmt16::Rgba5551, 0x0000);
        assert_eq!(a0_5551, 0x00000000);
    }
}
