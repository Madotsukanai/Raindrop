// DirectDraw surface engine: sysmem surfaces, software blits, texture upload
// into the raindrop-d3d9 wgpu device, and the "blit into the 3D frame" path.
// Faithful port of the C D7 emu (D7Surf / D7_Blit / D7_GetTex / D7_BltToDev).

use crate::com::*;
use crate::fmt::*;
use crate::state::emu;
use core::ffi::c_void;
use raindrop_d3d9::d3d9::*;
use raindrop_d3d9::types_gen::{D3DPRESENT_PARAMETERS, D3DVIEWPORT9};
use raindrop_d3d9::vtbl::Texture;

pub const DDSCAPS_PRIMARYSURFACE: u32 = 512;
pub const DDSCAPS_BACKBUFFER: u32 = 4;
pub const DDSCAPS_3DDEVICE: u32 = 8192;
pub const DDSCAPS_VIDEOMEMORY: u32 = 16384;
pub const DDSCAPS_ZBUFFER: u32 = 131072;
pub const DDBLT_COLORFILL: u32 = 1024;
pub const DDBLT_KEYSRC: u32 = 32768;
pub const DDBLT_DEPTHFILL: u32 = 33554432;
pub const DDBLTFAST_SRCCOLORKEY: u32 = 1;

#[derive(Clone, Copy, Default, PartialEq)]
pub struct Rect {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}

impl Rect {
    pub fn w(&self) -> i32 { self.right - self.left }
    pub fn h(&self) -> i32 { self.bottom - self.top }
    pub fn full(w: i32, h: i32) -> Rect { Rect { left: 0, top: 0, right: w, bottom: h } }
}

/// The COM object the game holds for an IDirectDrawSurface7.
#[repr(C)]
pub struct SurfObj {
    pub vt: *mut *mut c_void,
    pub refs: i32,
    pub caps: u32,
    pub w: i32,
    pub h: i32,
    pub bpp: i32,
    pub pitch: usize,
    pub fmt: Fmt,
    pub rgb_bit_count: u32,
    pub data: Vec<u8>,
    pub has_key: bool,
    pub key_lo: u32,
    pub dirty: bool,
    pub dpart: bool,
    pub drc: Rect,
    pub lock_ro: bool,
    pub lock_full: bool,
    pub lock_rc: Rect,
    pub tex: Option<*mut raindrop_d3d9::com::ComObj>,
    pub is_primary: bool,
    pub back: Option<*mut SurfObj>,
    pub dib: isize,
}

impl SurfObj {
    pub fn new(w: i32, h: i32, rgb_bit_count: u32, r: u32, g: u32, b: u32, a: u32, caps: u32) -> Option<Box<SurfObj>> {
        if rgb_bit_count != 16 && rgb_bit_count != 32 {
            return None;
        }
        let pitch = ((w as usize * rgb_bit_count as usize + 31) / 32) * 4;
        let s = SurfObj {
            vt: core::ptr::null_mut(),
            refs: 1,
            caps,
            w,
            h,
            bpp: rgb_bit_count as i32,
            pitch,
            fmt: Fmt::from_pf(rgb_bit_count, r, g, b, a),
            rgb_bit_count,
            data: vec![0; pitch * h as usize],
            has_key: false,
            key_lo: 0,
            dirty: true,
            dpart: false,
            drc: Rect::default(),
            lock_ro: false,
            lock_full: true,
            lock_rc: Rect::default(),
            tex: None,
            is_primary: false,
            back: None,
            dib: 0,
        };
        Some(Box::new(s))
    }

    pub fn mark_dirty(&mut self, r: Option<&Rect>) {
        match r {
            None => {
                self.dirty = true;
                self.dpart = false;
            }
            Some(r) => {
                let mut c = *r;
                c.left = c.left.max(0);
                c.top = c.top.max(0);
                c.right = c.right.min(self.w);
                c.bottom = c.bottom.min(self.h);
                if c.right <= c.left || c.bottom <= c.top {
                    return;
                }
                if !self.dirty {
                    self.dirty = true;
                    self.dpart = true;
                    self.drc = c;
                } else if self.dpart {
                    self.drc.left = self.drc.left.min(c.left);
                    self.drc.top = self.drc.top.min(c.top);
                    self.drc.right = self.drc.right.max(c.right);
                    self.drc.bottom = self.drc.bottom.max(c.bottom);
                }
            }
        }
    }

    fn rd(&self, off: usize) -> u32 {
        if self.bpp == 32 {
            u32::from_le_bytes(self.data[off..off + 4].try_into().unwrap())
        } else {
            u16::from_le_bytes(self.data[off..off + 2].try_into().unwrap()) as u32
        }
    }

    fn wr(&mut self, off: usize, v: u32) {
        if self.bpp == 32 {
            self.data[off..off + 4].copy_from_slice(&v.to_le_bytes());
        } else {
            self.data[off..off + 2].copy_from_slice(&(v as u16).to_le_bytes());
        }
    }
}

/// Software blit (stretch + color key + format conversion), C-parity.
pub fn blit(d: *mut SurfObj, dr_in: Option<Rect>, s: *mut SurfObj, sr_in: Option<Rect>, use_key: bool) {
    unsafe {
        let d = &mut *d;
        let s = &mut *s;
        if d.bpp < 16 || s.bpp < 16 {
            return;
        }
        let dr = dr_in.unwrap_or_else(|| Rect::full(d.w, d.h));
        let sr = sr_in.unwrap_or_else(|| Rect::full(s.w, s.h));
        let (sw, sh, dw, dh) = (sr.w(), sr.h(), dr.w(), dr.h());
        if sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 {
            return;
        }
        let (sb, db) = ((s.bpp / 8) as usize, (d.bpp / 8) as usize);
        let same = s.fmt.same(&d.fmt);
        for y in 0..dh {
            let dy = dr.top + y;
            let sy = sr.top + ((y as i64 * sh as i64) / dh as i64) as i32;
            if dy < 0 || dy >= d.h || sy < 0 || sy >= s.h {
                continue;
            }
            let drow = (dy as usize) * d.pitch;
            let srow = (sy as usize) * s.pitch;
            if same && !use_key && sw == dw && dr.left >= 0 && dr.left + dw <= d.w && sr.left >= 0 && sr.left + sw <= s.w {
                let src = srow + (sr.left as usize) * sb;
                let dst = drow + (dr.left as usize) * db;
                let n = (dw as usize) * db;
                d.data[dst..dst + n].copy_from_slice(&s.data[src..src + n]);
                continue;
            }
            for x in 0..dw {
                let dx = dr.left + x;
                let sx = sr.left + ((x as i64 * sw as i64) / dw as i64) as i32;
                if dx < 0 || dx >= d.w || sx < 0 || sx >= s.w {
                    continue;
                }
                let raw = s.rd(srow + (sx as usize) * sb);
                if use_key && s.has_key && (raw & !s.fmt.m[3]) == (s.key_lo & !s.fmt.m[3]) {
                    continue;
                }
                let val = if same { raw } else { d.fmt.argb_to_raw(s.fmt.raw_to_argb(raw)) };
                d.wr(drow + (dx as usize) * db, val);
            }
        }
        d.mark_dirty(Some(&dr));
    }
}

pub fn fill(d: *mut SurfObj, r_in: Option<Rect>, raw: u32) {
    unsafe {
        let d = &mut *d;
        if d.bpp < 16 {
            return;
        }
        let mut r = r_in.unwrap_or_else(|| Rect::full(d.w, d.h));
        r.left = r.left.max(0);
        r.top = r.top.max(0);
        r.right = r.right.min(d.w);
        r.bottom = r.bottom.min(d.h);
        let db = (d.bpp / 8) as usize;
        for y in r.top..r.bottom {
            let row = (y as usize) * d.pitch;
            for x in r.left..r.right {
                d.wr(row + (x as usize) * db, raw);
            }
        }
        d.mark_dirty(Some(&r));
    }
}

/// Create (once) and upload the DDraw surface's pixels into the wgpu device as
/// an A8R8G8B8 texture. Pixels are written straight into the d3d9 texture's
/// system-memory level; the renderer flushes it once per frame.
pub fn get_tex(s: *mut SurfObj) -> Option<*mut raindrop_d3d9::com::ComObj> {
    unsafe {
        let e = emu();
        let s = &mut *s;
        if s.bpp != 16 && s.bpp != 32 {
            return None;
        }
        if s.tex.is_none() {
            let dev = e.dev()?;
            let mut out: *mut core::ffi::c_void = core::ptr::null_mut();
            let hr = raindrop_d3d9::device::create_texture(
                dev, s.w as u32, s.h as u32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                &mut out, core::ptr::null_mut(),
            );
            if hr < 0 || out.is_null() {
                return None;
            }
            s.tex = Some(out as *mut raindrop_d3d9::com::ComObj);
            s.dirty = true;
            s.dpart = false;
        }
        if s.dirty {
            let texobj = s.tex.unwrap();
            let t = &mut *((*texobj).data as *mut Texture);
            let (w, h) = (s.w as usize, s.h as usize);
            let dst = &mut t.levels_sys[0];
            if dst.data.len() < w * h * 4 {
                return Some(texobj);
            }
            let rc = if s.dpart { s.drc } else { Rect::full(s.w, s.h) };
            let bytes = (s.bpp / 8) as usize;
            let n = rc.w().max(0) as usize;
            let hgt = rc.h().max(0) as usize;
            if n > 0 && hgt > 0 && rc.top >= 0 && rc.top < s.h {
                for y in rc.top..rc.bottom {
                    let sy = y as usize;
                    let base = sy * s.pitch + (rc.left as usize) * bytes;
                    let sp = &s.data[base..base + n * bytes];
                    let dp = &mut dst.data[sy * w * 4..];
                    if s.bpp == 32 && s.fmt.m[0] == 0x00FF0000 && s.fmt.m[1] == 0x0000FF00 && s.fmt.m[2] == 0x000000FF && (s.fmt.m[3] == 0 || s.fmt.m[3] == 0xFF000000) {
                        let add_a = if s.fmt.m[3] == 0 { 0xFF000000u32 } else { 0 };
                        let key = if s.has_key {
                            Some((s.key_lo & !s.fmt.m[3], !s.fmt.m[3]))
                        } else {
                            None
                        };
                        raindrop_gpu::kernels::upload_xrgb32(
                            core::slice::from_raw_parts_mut(dp.as_mut_ptr() as *mut u32, n),
                            core::slice::from_raw_parts(sp.as_ptr() as *const u32, n),
                            add_a,
                            key,
                        );
                    } else if s.bpp == 16 && fmt_code(&s.fmt).is_some() {
                        let key16 = if s.has_key && (s.key_lo & !s.fmt.m[3]) <= 0xFFFF {
                            Some(((s.key_lo & !s.fmt.m[3]) as u16, !(s.fmt.m[3] as u16)))
                        } else {
                            None
                        };
                        let src16: Vec<u16> = sp.chunks_exact(2).map(|c| u16::from_le_bytes([c[0], c[1]])).collect();
                        raindrop_gpu::kernels::upload_conv16(
                            core::slice::from_raw_parts_mut(dp.as_mut_ptr() as *mut u32, n),
                            &src16,
                            fmt_code(&s.fmt).unwrap(),
                            key16,
                        );
                    } else {
                        for x in 0..n {
                            let raw = s.rd(sy * s.pitch + (rc.left as usize) * bytes + x * bytes);
                            let mut argb = s.fmt.raw_to_argb(raw);
                            if s.has_key && (raw & !s.fmt.m[3]) == (s.key_lo & !s.fmt.m[3]) {
                                argb &= 0x00FFFFFF;
                            }
                            dp[x * 4..x * 4 + 4].copy_from_slice(&argb.to_le_bytes());
                        }
                    }
                }
                dst.dirty = true;
            }
            s.dirty = false;
            s.dpart = false;
        }
        s.tex
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Quad {
    x: f32,
    y: f32,
    z: f32,
    rhw: f32,
    color: u32,
    u: f32,
    v: f32,
}

/// Draw a DDraw blit as a textured quad into the wgpu scene (C D7_BltToDev).
pub fn blt_to_dev(dr_in: Option<Rect>, s: *mut SurfObj, sr_in: Option<Rect>, use_key: bool) -> bool {
    unsafe {
        let e = emu();
        let Some(dev) = e.dev() else { return false };
        let s = &mut *s;
        let dr = dr_in.unwrap_or_else(|| Rect::full(e.disp_w, e.disp_h));
        let sr = sr_in.unwrap_or_else(|| Rect::full(s.w, s.h));
        if sr.w() <= 0 || sr.h() <= 0 || dr.w() <= 0 || dr.h() <= 0 {
            return false;
        }
        if sr.left < 0 || sr.top < 0 || sr.right > s.w || sr.bottom > s.h {
            return false;
        }
        let Some(t) = get_tex(s) else { return false };
        let x0 = dr.left as f32 - 0.5;
        let y0 = dr.top as f32 - 0.5;
        let x1 = dr.right as f32 - 0.5;
        let y1 = dr.bottom as f32 - 0.5;
        let u0 = sr.left as f32 / s.w as f32;
        let v0 = sr.top as f32 / s.h as f32;
        let u1 = sr.right as f32 / s.w as f32;
        let v1 = sr.bottom as f32 / s.h as f32;
        let mut q = [Quad { x: 0.0, y: 0.0, z: 0.0, rhw: 1.0, color: 0xFFFFFFFF, u: 0.0, v: 0.0 }; 4];
        q[0].x = x0; q[0].y = y0; q[0].u = u0; q[0].v = v0;
        q[1].x = x1; q[1].y = y0; q[1].u = u1; q[1].v = v0;
        q[2].x = x0; q[2].y = y1; q[2].u = u0; q[2].v = v1;
        q[3].x = x1; q[3].y = y1; q[3].u = u1; q[3].v = v1;

        // saved state (12 rs + 6 tss + 5 smp, C-parity)
        const NRS: usize = 12;
        let rs_ids = [
            D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_LIGHTING, D3DRS_CULLMODE, D3DRS_ALPHABLENDENABLE,
            D3DRS_FOGENABLE, D3DRS_SPECULARENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE,
            D3DRS_ALPHATESTENABLE, D3DRS_ALPHAFUNC, D3DRS_ALPHAREF,
        ];
        let rs_val: [u32; NRS] = [
            0, 0, 0, D3DCULL_NONE, 0, 0, 0, 0, 0,
            if use_key { 1 } else { 0 }, D3DCMP_GREATER, 0,
        ];
        let d = &*((*dev).data as *mut raindrop_d3d9::device::Device);
        let rs_old: [u32; NRS] = core::array::from_fn(|i| d.rs[rs_ids[i] as usize]);
        let tss_ids: [(u32, u32); 6] = [(0, 1), (0, 2), (0, 4), (0, 5), (1, 1), (1, 4)];
        let tss_val: [u32; 6] = [
            D3DTOP_SELECTARG1, D3DTA_TEXTURE, D3DTOP_SELECTARG1, D3DTA_TEXTURE,
            D3DTOP_DISABLE, D3DTOP_DISABLE,
        ];
        let tss_old: [u32; 6] = core::array::from_fn(|i| d.tss[tss_ids[i].0 as usize][tss_ids[i].1 as usize]);
        let smp_ids: [u32; 5] = [D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV];
        let smp_val: [u32; 5] = [D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP];
        let smp_old: [u32; 5] = core::array::from_fn(|i| d.smp[0][smp_ids[i] as usize]);
        let old_tex0 = d.textures_bound[0];
        let old_fvf = d.fvf;
        e.flush_batch();
        let was_scene = e.in_scene;
        if !was_scene {
            raindrop_d3d9::device::begin_scene(dev);
        }
        for i in 0..NRS {
            raindrop_d3d9::device::set_render_state(dev, rs_ids[i], rs_val[i]);
        }
        for (i, (st, ty)) in tss_ids.iter().enumerate() {
            raindrop_d3d9::device::set_texture_stage_state(dev, *st, *ty, tss_val[i]);
        }
        for (i, ty) in smp_ids.iter().enumerate() {
            raindrop_d3d9::device::set_sampler_state(dev, 0, *ty, smp_val[i]);
        }
        raindrop_d3d9::device::set_texture(dev, 0, t);
        raindrop_d3d9::device::set_fvf(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        raindrop_d3d9::device::draw_primitive_up(dev, D3DPT_TRIANGLESTRIP, 2, q.as_ptr() as *const core::ffi::c_void, 28);
        // restore
        for i in 0..NRS {
            raindrop_d3d9::device::set_render_state(dev, rs_ids[i], rs_old[i]);
        }
        for (i, (st, ty)) in tss_ids.iter().enumerate() {
            raindrop_d3d9::device::set_texture_stage_state(dev, *st, *ty, tss_old[i]);
        }
        for (i, ty) in smp_ids.iter().enumerate() {
            raindrop_d3d9::device::set_sampler_state(dev, 0, *ty, smp_old[i]);
        }
        raindrop_d3d9::device::set_texture(dev, 0, old_tex0.unwrap_or_else(core::ptr::null_mut));
        if old_fvf != 0 {
            raindrop_d3d9::device::set_fvf(dev, old_fvf);
        }
        if !was_scene {
            raindrop_d3d9::device::end_scene(dev);
        }
        true
    }
}

/// Read the wgpu scene back into the DDraw surface's sysmem (the GetDC path).
pub fn sync_scene_to_surf(s: *mut SurfObj) {
    unsafe {
        let e = emu();
        let Some(dev) = e.dev() else { return };
        let s = &mut *s;
        if s.data.is_empty() || (s.bpp != 16 && s.bpp != 32) {
            return;
        }
        if e.back3d != Some(s) && !s.is_primary {
            return;
        }
        let mut back: *mut core::ffi::c_void = core::ptr::null_mut();
        if raindrop_d3d9::device::get_back_buffer(dev, 0, 0, 0, &mut back) < 0 || back.is_null() {
            return;
        }
        let mut rb: *mut core::ffi::c_void = core::ptr::null_mut();
        let backobj = back as *mut raindrop_d3d9::com::ComObj;
        let bs = &*((*backobj).data as *mut raindrop_d3d9::vtbl::Surface);
        let (bw, bh) = (bs.w, bs.h);
        let hr1 = raindrop_d3d9::device::create_offscreen_surface(dev, bw, bh, D3DFMT_X8R8G8B8, 2, &mut rb, core::ptr::null_mut());
        if hr1 < 0 || rb.is_null() {
            return;
        }
        let rbobj = rb as *mut raindrop_d3d9::com::ComObj;
        let hr2 = raindrop_d3d9::device::get_render_target_data(dev, backobj, rbobj);
        if hr2 < 0 {
            return;
        }
        let rbs = &*((*(rb as *mut raindrop_d3d9::com::ComObj)).data as *mut raindrop_d3d9::vtbl::Surface);
        let Some(sys) = &rbs.sys else { return };
        let (w, h) = (s.w, s.h);
        let dst_fmt = s.fmt;
        for y in 0..h {
            let sy = ((y as i64 * bh as i64) / h as i64) as usize;
            let src_row = &sys.data[sy * sys.pitch..];
            for x in 0..w {
                let sx = ((x as i64 * bw as i64) / w as i64) as usize;
                let c = u32::from_le_bytes(src_row[sx * 4..sx * 4 + 4].try_into().unwrap()) | 0xFF000000;
                s.wr((y as usize) * s.pitch + x as usize * (s.bpp / 8) as usize, dst_fmt.argb_to_raw(c));
            }
        }
        s.mark_dirty(None);
    }
}

/// Create the d3d9 device on demand (C D7_InitD3D9, minus C-only helpers).
pub fn init_d3d9(hwnd: isize, interval: u32) -> bool {
    unsafe {
        let e = emu();
        if e.dev.is_some() {
            return true;
        }
        let Some(d3d9) = e.d3d9 else { return false };
        if hwnd == 0 {
            return false;
        }
        let mut pp = D3DPRESENT_PARAMETERS {
            BackBufferWidth: e.disp_w.max(1) as u32,
            BackBufferHeight: e.disp_h.max(1) as u32,
            BackBufferFormat: D3DFMT_X8R8G8B8,
            BackBufferCount: 1,
            MultiSampleType: 0,
            MultiSampleQuality: 0,
            SwapEffect: D3DSWAPEFFECT_DISCARD,
            Windowed: 1,
            EnableAutoDepthStencil: 0,
            AutoDepthStencilFormat: 0,
            Flags: 0,
            FullScreen_RefreshRateInHz: 0,
            PresentationInterval: interval,
            hDeviceWindow: hwnd as *mut core::ffi::c_void,
        };
        let mut out: *mut core::ffi::c_void = core::ptr::null_mut();
        let mut hr = raindrop_d3d9::d9::create_device(d3d9, 0, 1 /*HAL*/, hwnd as *mut core::ffi::c_void,
            0x10 | 0x40 | 0x2 /* PURE | HW VP | FPU_PRESERVE */, &mut pp, &mut out);
        if hr < 0 {
            hr = raindrop_d3d9::d9::create_device(d3d9, 0, 1, hwnd as *mut core::ffi::c_void,
                0x40 | 0x2, &mut pp, &mut out);
        }
        if hr < 0 {
            hr = raindrop_d3d9::d9::create_device(d3d9, 0, 1, hwnd as *mut core::ffi::c_void,
                0x20 | 0x2, &mut pp, &mut out);
        }
        if hr < 0 || out.is_null() {
            return false;
        }
        e.dev = Some(out as *mut raindrop_d3d9::com::ComObj);
        let dev = out as *mut raindrop_d3d9::com::ComObj;
        raindrop_d3d9::device::set_render_state(dev, D3DRS_ZENABLE, 0);
        raindrop_d3d9::device::set_render_state(dev, D3DRS_LIGHTING, 0);
        true
    }
}
