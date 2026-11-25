// COM vtables and slot implementations for IDirectDraw7, IDirectDrawSurface7,
// IDirect3D7 and IDirect3DDevice7 — a faithful port of the C D7 emu in
// src/Smd/Smd_d3d9.c, talking to the raindrop-d3d9 wgpu device directly.

use crate::com::*;
use crate::com::{singleton_addref, singleton_release};
use crate::state::{emu, emu_ready};
use crate::fmt::{DDPF_ALPHAPIXELS, DDPF_RGB, DDPF_ZBUFFER};
use crate::surf::*;
use core::ffi::c_void;
use raindrop_d3d9::d3d9::*;
use raindrop_d3d9::types_gen::{D3DPRESENT_PARAMETERS, D3DVIEWPORT9, RECT};

// ---- IIDs (little-endian GUID bytes, from mingw dxguid) -------------------
pub const IID_UNKNOWN: [u8; 16] = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
pub const IID_DD7: [u8; 16] = [0xC0, 0x5E, 0xE6, 0x15, 0x9C, 0x3B, 0xD2, 0x11, 0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B];
pub const IID_DD1: [u8; 16] = [0x6C, 0x14, 0x78, 0x6C, 0x2C, 0x3B, 0xD2, 0x11, 0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B];
pub const IID_DD2: [u8; 16] = [0x65, 0x7A, 0x6A, 0x6C, 0x2C, 0x3B, 0xD2, 0x11, 0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B];
pub const IID_DD3: [u8; 16] = [0x3C, 0x71, 0xA4, 0x6E, 0x2C, 0x3B, 0xD2, 0x11, 0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B];
pub const IID_DD4: [u8; 16] = [0x9C, 0x59, 0x59, 0x7C, 0x2C, 0x3B, 0xD2, 0x11, 0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B];
pub const IID_SURF7: [u8; 16] = [0x80, 0x5A, 0x67, 0x06, 0x9B, 0x3B, 0xD2, 0x11, 0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B];
pub const IID_D3D7: [u8; 16] = [0x77, 0x9E, 0x04, 0xF5, 0x61, 0x48, 0xD2, 0x11, 0xA4, 0x07, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8];
pub const IID_DEV7: [u8; 16] = [0x79, 0x9E, 0x04, 0xF5, 0x61, 0x48, 0xD2, 0x11, 0xA4, 0x07, 0x00, 0xA0, 0xC9, 0x06, 0x29, 0xA8];
pub const IID_HAL_DEVICE: [u8; 16] = [0xE0, 0x3D, 0xE6, 0x84, 0xAA, 0x46, 0xCF, 0x11, 0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E];

// ---- struct offsets (verified against mingw headers) ----------------------
const SD_FLAGS: usize = 4;
const SD_HEIGHT: usize = 8;
const SD_WIDTH: usize = 12;
const SD_PITCH: usize = 16;
const SD_BACKCOUNT: usize = 20;
const SD_LPSURFACE: usize = 36;
const SD_PF: usize = 68;
const SD_CAPS: usize = 104;
const PF_FLAGS: usize = 4;
const PF_BITCOUNT: usize = 12;
const PF_R: usize = 16;
const FX_FILLCOLOR: usize = 80;
const DI_SZDESCRIPTION: usize = 512;

// ---- D3D7 constants not present in raindrop-d3d9 --------------------------
pub const D3DERR_DEVICELOST: i32 = 0x88760868u32 as i32;
pub const D3DERR_DEVICENOTRESET: i32 = 0x88760869u32 as i32;
pub const D3DSAMP_BORDERCOLOR: u32 = 15;
pub const D3DSAMP_MIPMAPLODBIAS: u32 = 19;
pub const D3DSAMP_MAXMIPLEVEL: u32 = 20;
pub const D3DSAMP_MAXANISOTROPY: u32 = 21;
pub const D3DVIEWPORT_X: usize = 0;
pub const D3DVIEWPORT_W: usize = 8;
pub const D3DVIEWPORT_H: usize = 12;
pub const D3DVIEWPORT_MINZ: usize = 32;
pub const D3DVIEWPORT_MAXZ: usize = 36;

pub type V = unsafe extern "system" fn();

pub struct Vtables {
    pub surf: *mut *mut c_void,
    pub dd7: *mut *mut c_void,
    pub d3d7: *mut *mut c_void,
    pub dev7: *mut *mut c_void,
}

static mut VT: Option<Vtables> = None;

pub fn all() -> Vtables {
    unsafe {
        if VT.is_none() {
            VT = Some(Vtables {
                surf: leak_slots(surf_vt()),
                dd7: leak_slots(dd7_vt()),
                d3d7: leak_slots(d3d7_vt()),
                dev7: leak_slots(dev7_vt()),
            });
        }
        VT.as_ref().unwrap().clone_struct()
    }
}

impl Vtables {
    fn clone_struct(&self) -> Vtables {
        Vtables { surf: self.surf, dd7: self.dd7, d3d7: self.d3d7, dev7: self.dev7 }
    }
}

macro_rules! slots {
    ($($f:ident),+ $(,)?) => {
        vec![$($f as *mut c_void),+]
    };
}

// ---------------------------------------------------------------------------
// IDirectDrawSurface7 (49 slots, exact order from the C emu)
// ---------------------------------------------------------------------------

fn surf_vt() -> Vec<*mut c_void> {
    slots![
        es_qi, es_addref, es_release, es_add_attached_surface, es_add_overlay_dirty_rect,
        es_blt, es_blt_batch, es_blt_fast, es_delete_attached_surface, es_enum_attached_surfaces,
        es_enum_overlay_z_orders, es_flip, es_get_attached_surface, es_get_blt_status,
        es_get_caps, es_get_clipper, es_get_color_key, es_get_dc, es_get_flip_status,
        es_get_overlay_position, es_get_palette, es_get_pixel_format, es_get_surface_desc,
        es_initialize, es_is_lost, es_lock, es_release_dc, es_restore, es_set_clipper,
        es_set_color_key, es_set_overlay_position, es_set_palette, es_unlock, es_update_overlay,
        es_update_overlay_display, es_update_overlay_z_order, es_get_dd_interface, es_page_lock,
        es_page_unlock, es_set_surface_desc, es_set_private_data, es_get_private_data,
        es_free_private_data, es_get_uniqueness_value, es_change_uniqueness_value,
        es_set_priority, es_get_priority, es_set_lod, es_get_lod,
    ]
}

unsafe fn iid_matches(p: *mut c_void, iid: &[u8; 16]) -> bool {
    let b = p as *const u8;
    (0..16).all(|i| *b.add(i) == iid[i])
}

unsafe extern "system" fn es_qi(s: *mut c_void, riid: *mut c_void, out: *mut c_void) -> Hr {
    let out = out as *mut *mut c_void;
    if out.is_null() { return E_POINTER; }
    *out = core::ptr::null_mut();
    if iid_matches(riid, &IID_UNKNOWN) || iid_matches(riid, &IID_SURF7) {
        es_addref(s);
        *out = s;
        return DD_OK;
    }
    E_NOINTERFACE
}

unsafe extern "system" fn es_addref(s: *mut c_void) -> u32 {
    let sp = s as *mut SurfObj;
    (*sp).refs += 1;
    (*sp).refs as u32
}

unsafe extern "system" fn es_release(s: *mut c_void) -> u32 {
    let sp = s as *mut SurfObj;
    let s = &mut *sp;
    s.refs -= 1;
    if s.refs > 0 {
        return s.refs as u32;
    }
    let e = emu();
    if e.primary == Some(sp) { e.primary = None; }
    if e.back3d == Some(sp) { e.back3d = None; }
    for i in 0..8 {
        if e.cur_tex[i] == Some(sp) { e.cur_tex[i] = None; }
    }
    crate::win::dib_drop(sp);
    drop(Box::from_raw(sp));
    0
}

unsafe extern "system" fn es_add_attached_surface(s: *mut c_void, _f: u32, att: *mut c_void) -> Hr {
    let _ = (s, att);
    DD_OK // z-buffer chains ignored (no depth buffering in the emu)
}

unsafe extern "system" fn es_blt(
    s: *mut c_void, dr: *const RECT, src: *mut c_void, sr: *const RECT,
    flags: u32, fx: *mut c_void,
) -> Hr {
    let e = emu();
    let d = s as *mut SurfObj;
    let sp = if src.is_null() { core::ptr::null_mut() } else { src as *mut SurfObj };
    if (*d).is_primary {
        if !sp.is_null() && (e.back3d == Some(sp) || (*sp).caps & DDSCAPS_3DDEVICE != 0) {
            present_now();
        } else if !sp.is_null() && e.dev.is_some() {
            if !blt_to_dev(opt_rect(d, dr), sp, opt_rect(sp, sr), flags & DDBLT_KEYSRC != 0) {
                crate::win::log("D3D7 emu: Blt to the primary surface failed (invalid rectangle or texture)");
            }
            present_now();
        }
        return DD_OK;
    }
    if e.back3d == Some(d) && e.dev.is_some() {
        if flags & DDBLT_COLORFILL != 0 && !fx.is_null() {
            let raw = *(fx as *const u32).add(FX_FILLCOLOR / 4);
            let argb = (*d).fmt.raw_to_argb(raw) | 0xFF000000;
            let (n, rc) = match opt_rect(d, dr) {
                Some(r) => (1u32, RECT { left: r.left, top: r.top, right: r.right, bottom: r.bottom }),
                None => (0u32, RECT { left: 0, top: 0, right: 0, bottom: 0 }),
            };
            raindrop_d3d9::device::clear(e.dev.unwrap(), n, &rc, D3DCLEAR_TARGET, argb, 1.0, 0);
        } else if !sp.is_null() && sp != d && flags & DDBLT_DEPTHFILL == 0 {
            if !blt_to_dev(opt_rect(d, dr), sp, opt_rect(sp, sr), flags & DDBLT_KEYSRC != 0) {
                crate::win::log("D3D7 emu: Blt into the 3D back buffer failed");
            }
        }
        return DD_OK;
    }
    if flags & DDBLT_COLORFILL != 0 && !fx.is_null() {
        let raw = *(fx as *const u32).add(FX_FILLCOLOR / 4);
        fill(d, opt_rect(d, dr), raw);
    } else if !sp.is_null() {
        blit(d, opt_rect(d, dr), sp, opt_rect(sp, sr), flags & DDBLT_KEYSRC != 0);
    }
    DD_OK
}

unsafe extern "system" fn es_blt_batch(_s: *mut c_void, _b: *mut c_void, _n: u32, _f: u32) -> Hr { DD_OK }

unsafe extern "system" fn es_blt_fast(
    s: *mut c_void, x: u32, y: u32, src: *mut c_void, sr: *const RECT, trans: u32,
) -> Hr {
    let e = emu();
    let d = s as *mut SurfObj;
    let sp = if src.is_null() { core::ptr::null_mut() } else { src as *mut SurfObj };
    if sp.is_null() { return DDERR_INVALIDPARAMS; }
    let src_r = match opt_rect(sp, sr) {
        Some(r) => r,
        None => Rect::full((*sp).w, (*sp).h),
    };
    let dr = Rect { left: x as i32, top: y as i32, right: x as i32 + src_r.w(), bottom: y as i32 + src_r.h() };
    if e.dev.is_some() && sp != d && (e.back3d == Some(d) || (*d).is_primary) {
        if !blt_to_dev(Some(dr), sp, Some(src_r), trans & DDBLTFAST_SRCCOLORKEY != 0) {
            crate::win::log("D3D7 emu: BltFast into the 3D back buffer / primary failed");
        }
        if (*d).is_primary {
            present_now();
        }
        return DD_OK;
    }
    blit(d, Some(dr), sp, Some(src_r), trans & DDBLTFAST_SRCCOLORKEY != 0);
    DD_OK
}

unsafe extern "system" fn es_flip(s: *mut c_void, _t: *mut c_void, _f: u32) -> Hr {
    if (*(s as *mut SurfObj)).is_primary {
        present_now();
    }
    DD_OK
}

unsafe extern "system" fn es_get_attached_surface(s: *mut c_void, caps: *const u32, out: *mut c_void) -> Hr {
    let _ = s;
    let out = out as *mut *mut c_void;
    *out = core::ptr::null_mut();
    DDERR_NOTFOUND
}

unsafe extern "system" fn es_get_blt_status(_s: *mut c_void, _f: u32) -> Hr { DD_OK }
unsafe extern "system" fn es_get_flip_status(_s: *mut c_void, _f: u32) -> Hr { DD_OK }

unsafe extern "system" fn es_get_caps(s: *mut c_void, c: *mut u32) -> Hr {
    if c.is_null() { return DDERR_INVALIDPARAMS; }
    // DDSCAPS2: dwCaps, dwCaps2, dwCaps3, dwCaps4 -> we only write dwCaps,
    // caller memsets first (DD7 docs); to be safe zero all four.
    let c = c as *mut u32;
    *c.add(0) = (*(s as *mut SurfObj)).caps;
    *c.add(1) = 0;
    *c.add(2) = 0;
    *c.add(3) = 0;
    DD_OK
}

unsafe extern "system" fn es_get_color_key(s: *mut c_void, _f: u32, k: *mut c_void) -> Hr {
    let s = &*(s as *mut SurfObj);
    if k.is_null() { return DDERR_INVALIDPARAMS; }
    if !s.has_key { return DDERR_NOCOLORKEY; }
    let k = k as *mut u32;
    *k.add(0) = s.key_lo;
    *k.add(1) = s.key_lo;
    DD_OK
}

unsafe extern "system" fn es_set_color_key(s: *mut c_void, _f: u32, k: *mut c_void) -> Hr {
    let s = &mut *(s as *mut SurfObj);
    if k.is_null() {
        s.has_key = false;
        s.key_lo = 0;
        s.dirty = true;
        return DD_OK;
    }
    let k = k as *const u32;
    s.has_key = true;
    s.key_lo = *k.add(0);
    s.dirty = true;
    DD_OK
}

unsafe extern "system" fn es_get_dc(s: *mut c_void, out: *mut c_void) -> Hr {
    let out = out as *mut isize;
    if out.is_null() { return DDERR_INVALIDPARAMS; }
    let sp = s as *mut SurfObj;
    sync_scene_to_surf(sp);
    let Some(hdc) = crate::win::dib_get(sp) else {
        return DDERR_CANTCREATEDC;
    };
    *out = hdc;
    DD_OK
}

unsafe extern "system" fn es_release_dc(s: *mut c_void, hdc: isize) -> Hr {
    let _ = s;
    crate::win::release_dc(hdc);
    DD_OK
}

unsafe extern "system" fn es_get_pixel_format(s: *mut c_void, pf: *mut c_void) -> Hr {
    let s = &*(s as *mut SurfObj);
    let p = pf as *mut u32;
    *p.add(0) = 32;
    *p.add(PF_FLAGS / 4) = DDPF_RGB | if s.fmt.m[3] != 0 { DDPF_ALPHAPIXELS } else { 0 };
    *p.add(PF_BITCOUNT / 4) = s.rgb_bit_count;
    *p.add(PF_R / 4) = s.fmt.m[0];
    *p.add(PF_R / 4 + 1) = s.fmt.m[1];
    *p.add(PF_R / 4 + 2) = s.fmt.m[2];
    *p.add(PF_R / 4 + 3) = s.fmt.m[3];
    DD_OK
}

unsafe extern "system" fn es_get_surface_desc(s: *mut c_void, desc: *mut c_void) -> Hr {
    let s = &*(s as *mut SurfObj);
    let d = desc as *mut u32;
    for i in 0..31 {
        *d.add(i) = 0;
    }
    *d.add(0) = 124; // sizeof(DDSURFACEDESC2)
    *d.add(SD_FLAGS / 4) = 0x1 | 0x2 | 0x4 | 0x1000; // CAPS|HEIGHT|WIDTH|PIXELFORMAT
    *d.add(SD_HEIGHT / 4) = s.h as u32;
    *d.add(SD_WIDTH / 4) = s.w as u32;
    *d.add(SD_PITCH / 4) = s.pitch as u32;
    *d.add(SD_FLAGS / 4) |= 0x8; // DDSD_PITCH
    let pf = d.add(SD_PF / 4);
    *pf.add(0) = 32;
    *pf.add(PF_FLAGS / 4) = DDPF_RGB | if s.fmt.m[3] != 0 { DDPF_ALPHAPIXELS } else { 0 };
    *pf.add(PF_BITCOUNT / 4) = s.rgb_bit_count;
    *pf.add(PF_R / 4) = s.fmt.m[0];
    *pf.add(PF_R / 4 + 1) = s.fmt.m[1];
    *pf.add(PF_R / 4 + 2) = s.fmt.m[2];
    *pf.add(PF_R / 4 + 3) = s.fmt.m[3];
    *d.add(SD_CAPS / 4) = s.caps;
    DD_OK
}

unsafe extern "system" fn es_lock(
    s: *mut c_void, r: *const RECT, desc: *mut c_void, flags: u32, _ev: *mut c_void,
) -> Hr {
    let sp = s as *mut SurfObj;
    let s = &mut *sp;
    if desc.is_null() { return DDERR_INVALIDPARAMS; }
    if (*s).data.is_empty() { return DDERR_SURFACELOST; }
    let mut ptr = (*s).data.as_mut_ptr();
    let mut full = true;
    let mut rc = Rect::full((*s).w, (*s).h);
    if !r.is_null() {
        let r = &*r;
        if r.left >= 0 && r.top >= 0 && r.left < (*s).w && r.top < (*s).h && r.right > r.left && r.bottom > r.top {
            rc = Rect { left: r.left, top: r.top, right: r.right, bottom: r.bottom };
            full = false;
            ptr = ptr.add((r.top as usize) * (*s).pitch + (r.left as usize) * ((*s).bpp / 8) as usize);
        }
    }
    (*s).lock_ro = flags & DDLOCK_READONLY != 0;
    (*s).lock_full = full;
    (*s).lock_rc = rc;
    let d = desc as *mut u32;
    es_get_surface_desc(sp as *mut core::ffi::c_void, desc);
    *d.add(SD_FLAGS / 4) |= 0x800; // DDSD_LPSURFACE
    *(d.add(SD_LPSURFACE / 4) as *mut *mut u8) = ptr;
    DD_OK
}

unsafe extern "system" fn es_unlock(s: *mut c_void, _r: *mut c_void) -> Hr {
    let s = &mut *(s as *mut SurfObj);
    if !s.lock_ro {
        if s.lock_full {
            s.mark_dirty(None);
        } else {
            let rc = s.lock_rc;
            s.mark_dirty(Some(&rc));
        }
    }
    s.lock_ro = false;
    s.lock_full = true;
    DD_OK
}

unsafe extern "system" fn es_restore(_s: *mut c_void) -> Hr { DD_OK }
unsafe extern "system" fn es_is_lost(_s: *mut c_void) -> Hr { DD_FALSE }
unsafe extern "system" fn es_initialize(_s: *mut c_void, _d: *mut c_void, _desc: *mut c_void) -> Hr { DDERR_ALREADYINITIALIZED }
unsafe extern "system" fn es_get_clipper(_s: *mut c_void, _c: *mut c_void) -> Hr { DDERR_NOCLIPPERATTACHED }
unsafe extern "system" fn es_set_clipper(_s: *mut c_void, _c: *mut c_void) -> Hr { DD_OK }
unsafe extern "system" fn es_get_palette(_s: *mut c_void, _p: *mut c_void) -> Hr { DDERR_NOPALETTEATTACHED }
unsafe extern "system" fn es_set_palette(_s: *mut c_void, _p: *mut c_void) -> Hr { DDERR_NOPALETTEATTACHED }
unsafe extern "system" fn es_get_overlay_position(_s: *mut c_void, _a: *mut c_void, _b: *mut c_void) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn es_set_overlay_position(_s: *mut c_void, _a: i32, _b: i32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn es_update_overlay(_s: *mut c_void, _d: *mut c_void, _f: u32, _fx: *mut c_void) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn es_update_overlay_display(_s: *mut c_void, _f: u32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn es_update_overlay_z_order(_s: *mut c_void, _f: u32, _o: *mut c_void) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn es_get_dd_interface(_s: *mut c_void, out: *mut c_void) -> Hr {
    let out = out as *mut *mut c_void;
    let e = emu();
    *out = e.obj_dd7 as *mut c_void;
    singleton_addref(e.obj_dd7);
    DD_OK
}
unsafe extern "system" fn es_page_lock(_s: *mut c_void, _f: u32) -> Hr { DD_OK }
unsafe extern "system" fn es_page_unlock(_s: *mut c_void, _f: u32) -> Hr { DD_OK }
unsafe extern "system" fn es_set_surface_desc(_s: *mut c_void, _d: *mut c_void, _f: u32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn es_set_private_data(_s: *mut c_void, _d: *mut c_void, _n: u32, _f: u32) -> Hr { DD_OK }
unsafe extern "system" fn es_get_private_data(_s: *mut c_void, _d: *mut c_void, _n: *mut u32) -> Hr { DDERR_NOTFOUND }
unsafe extern "system" fn es_free_private_data(_s: *mut c_void, _g: *mut c_void) -> Hr { DD_OK }
unsafe extern "system" fn es_get_uniqueness_value(_s: *mut c_void, _v: *mut u32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn es_change_uniqueness_value(_s: *mut c_void) -> Hr { DD_OK }
unsafe extern "system" fn es_set_priority(_s: *mut c_void, _p: u32) -> Hr { DD_OK }
unsafe extern "system" fn es_get_priority(_s: *mut c_void, _p: *mut u32) -> Hr { DD_OK }
unsafe extern "system" fn es_set_lod(_s: *mut c_void, _l: u32) -> Hr { DD_OK }
unsafe extern "system" fn es_get_lod(_s: *mut c_void, _l: *mut u32) -> Hr { DD_OK }
unsafe extern "system" fn es_add_overlay_dirty_rect(_s: *mut c_void, _r: *mut c_void) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn es_delete_attached_surface(_s: *mut c_void, _f: u32, _a: *mut c_void) -> Hr { DD_OK }
unsafe extern "system" fn es_enum_attached_surfaces(_s: *mut c_void, _c: *mut c_void, _cb: *mut c_void) -> Hr { DD_OK }
unsafe extern "system" fn es_enum_overlay_z_orders(_s: *mut c_void, _f: u32, _o: *mut c_void, _cb: *mut c_void) -> Hr { DDERR_UNSUPPORTED }

// ---------------------------------------------------------------------------
// IDirectDraw7 (30 slots)
// ---------------------------------------------------------------------------

fn dd7_vt() -> Vec<*mut c_void> {
    slots![
        ed_qi, ed_addref, ed_release, ed_compact, ed_create_clipper, ed_create_palette,
        ed_create_surface, ed_duplicate_surface, ed_enum_display_modes, ed_enum_surfaces,
        ed_flip_to_gdi_surface, ed_get_caps, ed_get_display_mode, ed_get_four_cc_codes,
        ed_get_gdi_surface, ed_get_monitor_frequency, ed_get_scan_line,
        ed_get_vertical_blank_status, ed_initialize, ed_restore_display_mode,
        ed_set_cooperative_level, ed_set_display_mode, ed_wait_for_vertical_blank,
        ed_get_available_vid_mem, ed_get_surface_from_dc, ed_restore_all_surfaces,
        ed_test_cooperative_level, ed_get_device_identifier, ed_start_mode_test,
        ed_evaluate_mode,
    ]
}

unsafe extern "system" fn ed_qi(_s: *mut c_void, riid: *mut c_void, out: *mut c_void) -> Hr {
    let out = out as *mut *mut c_void;
    if out.is_null() { return E_POINTER; }
    *out = core::ptr::null_mut();
    let e = emu();
    let obj = e.obj_dd7 as *mut c_void;
    if iid_matches(riid, &IID_UNKNOWN) || iid_matches(riid, &IID_DD7) || iid_matches(riid, &IID_DD1)
        || iid_matches(riid, &IID_DD2) || iid_matches(riid, &IID_DD3) || iid_matches(riid, &IID_DD4) {
        singleton_addref(e.obj_dd7);
        *out = obj;
        return DD_OK;
    }
    if iid_matches(riid, &IID_D3D7) {
        singleton_addref(e.obj_d3d7);
        *out = e.obj_d3d7 as *mut c_void;
        return DD_OK;
    }
    E_NOINTERFACE
}
unsafe extern "system" fn ed_addref(_s: *mut c_void) -> u32 { singleton_addref(emu().obj_dd7) }
unsafe extern "system" fn ed_release(_s: *mut c_void) -> u32 { singleton_release(emu().obj_dd7) }
unsafe extern "system" fn ed_compact(_s: *mut c_void) -> Hr { DD_OK }
unsafe extern "system" fn ed_create_clipper(_s: *mut c_void, _f: u32, _o: *mut c_void) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ed_create_palette(_s: *mut c_void, _f: u32, _p: *mut c_void, _o: *mut c_void) -> Hr { DDERR_UNSUPPORTED }

unsafe extern "system" fn ed_create_surface(
    _s: *mut c_void, desc: *mut c_void, out: *mut c_void, _unk: *mut c_void,
) -> Hr {
    if desc.is_null() { return DDERR_INVALIDPARAMS; }
    let out = out as *mut *mut c_void;
    if out.is_null() { return E_POINTER; }
    *out = core::ptr::null_mut();
    let e = emu();
    let d = desc as *const u32;
    let flags = *d.add(SD_FLAGS / 4);
    let caps = if flags & 0x1 != 0 { *d.add(SD_CAPS / 4) } else { 0 };
    let w = if flags & 0x4 != 0 { *d.add(SD_WIDTH / 4) as i32 } else { 0 };
    let h = if flags & 0x2 != 0 { *d.add(SD_HEIGHT / 4) as i32 } else { 0 };
    let (mut bpp, mut m) = (32u32, [0x00FF0000u32, 0x0000FF00, 0x000000FF, 0]);
    let mut is_z = false;
    let mut is_primary = false;
    if caps & DDSCAPS_PRIMARYSURFACE != 0 {
        is_primary = true;
    } else if flags & 0x1000 != 0 && (*d.add(SD_PF / 4 + PF_FLAGS / 4) & DDPF_ZBUFFER) != 0 {
        is_z = true;
    }
    if flags & 0x1000 != 0 {
        let pf = d.add(SD_PF / 4);
        if *pf.add(PF_FLAGS / 4) & DDPF_RGB != 0 {
            bpp = *pf.add(PF_BITCOUNT / 4);
            m[0] = *pf.add(PF_R / 4);
            m[1] = *pf.add(PF_R / 4 + 1);
            m[2] = *pf.add(PF_R / 4 + 2);
            m[3] = *pf.add(PF_R / 4 + 3);
        }
    }
    let (w, h) = if is_primary { (e.disp_w, e.disp_h) } else { (w, h) };
    if w <= 0 || h <= 0 {
        return DDERR_INVALIDPARAMS;
    }
    if !is_primary && !is_z && bpp != 16 && bpp != 32 {
        return DDERR_INVALIDPIXELFORMAT;
    }
    if is_z {
        // z-buffer surfaces are accepted but unused
        let s = SurfObj::new(w, h, bpp, m[0], m[1], m[2], m[3], caps | DDSCAPS_ZBUFFER);
        match s {
            Some(s) => {
                let mut s = s;
                s.vt = all().surf;
                *out = Box::into_raw(s) as *mut c_void;
                DD_OK
            }
            None => DDERR_OUTOFMEMORY,
        }
    } else {
        let caps2 = if is_primary {
            caps
        } else {
            caps | if is_z { DDSCAPS_ZBUFFER } else { 0 }
        };
        let s = SurfObj::new(w, h, bpp, m[0], m[1], m[2], m[3], caps2);
        let mut s = match s {
            Some(s) => s,
            None => return DDERR_OUTOFMEMORY,
        };
        s.vt = all().surf;
        if is_primary {
            s.is_primary = true;
            if flags & 0x20 != 0 && *d.add(SD_BACKCOUNT / 4) > 0 {
                // front/back chain: create a silent back buffer object
                let b = SurfObj::new(w, h, bpp, m[0], m[1], m[2], m[3],
                    DDSCAPS_BACKBUFFER | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY);
                if let Some(mut b) = b {
                    b.vt = all().surf;
                    s.back = Some(Box::into_raw(b));
                }
            }
            e.primary = Some(Box::into_raw(s));
            *out = e.primary.unwrap() as *mut c_void;
        } else {
            let p = Box::into_raw(s);
            if e.back3d.is_none() && caps & DDSCAPS_3DDEVICE != 0 {
                e.back3d = Some(p);
            }
            *out = p as *mut c_void;
        }
        DD_OK
    }
}

unsafe extern "system" fn ed_duplicate_surface(_s: *mut c_void, _a: *mut c_void, _o: *mut c_void) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ed_enum_display_modes(_s: *mut c_void, _f: u32, _d: *mut c_void, _c: *mut c_void, _cb: *mut c_void) -> Hr { DDENUMRET_OK }
unsafe extern "system" fn ed_enum_surfaces(_s: *mut c_void, _f: u32, _d: *mut c_void, _c: *mut c_void, _cb: *mut c_void) -> Hr { DDENUMRET_OK }
unsafe extern "system" fn ed_flip_to_gdi_surface(_s: *mut c_void) -> Hr { DD_OK }

unsafe extern "system" fn ed_get_caps(_s: *mut c_void, dc: *mut c_void, sc: *mut c_void) -> Hr {
    let c = dc as *mut u32;
    if !c.is_null() {
        *c.add(0) = 32;
        *c.add(1) = DDCAPS_3D | DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTSTRETCH | DDCAPS_GDI | DDCAPS_CANBLTSYSMEM;
    }
    let s = sc as *mut u32;
    if !s.is_null() {
        *s.add(0) = 32;
        *s.add(1) = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_BLTSTRETCH;
    }
    DD_OK
}

unsafe extern "system" fn ed_get_display_mode(_s: *mut c_void, desc: *mut c_void) -> Hr {
    let e = emu();
    if desc.is_null() { return DDERR_INVALIDPARAMS; }
    let d = desc as *mut u32;
    for i in 0..31 {
        *d.add(i) = 0;
    }
    *d.add(0) = 124;
    *d.add(SD_FLAGS / 4) = 0x2 | 0x4 | 0x1000 | 0x8;
    *d.add(SD_WIDTH / 4) = e.disp_w as u32;
    *d.add(SD_HEIGHT / 4) = e.disp_h as u32;
    *d.add(SD_PITCH / 4) = (e.disp_w * 4) as u32;
    let pf = d.add(SD_PF / 4);
    *pf.add(0) = 32;
    *pf.add(PF_FLAGS / 4) = DDPF_RGB;
    *pf.add(PF_BITCOUNT / 4) = 32;
    *pf.add(PF_R / 4) = 0x00FF0000;
    *pf.add(PF_R / 4 + 1) = 0x0000FF00;
    *pf.add(PF_R / 4 + 2) = 0x000000FF;
    *d.add(SD_CAPS / 4) = DDSCAPS_PRIMARYSURFACE | DDSCAPS_VIDEOMEMORY | DDSCAPS_3DDEVICE;
    DD_OK
}

unsafe extern "system" fn ed_get_four_cc_codes(_s: *mut c_void, _c: *mut u32, n: *mut u32) -> Hr {
    if !n.is_null() { *n = 0; }
    DD_OK
}
unsafe extern "system" fn ed_get_gdi_surface(_s: *mut c_void, _o: *mut c_void) -> Hr { DDERR_NOTFOUND }
unsafe extern "system" fn ed_get_monitor_frequency(_s: *mut c_void, f: *mut u32) -> Hr {
    if !f.is_null() { *f = 60; }
    DD_OK
}
unsafe extern "system" fn ed_get_scan_line(_s: *mut c_void, _l: *mut u32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ed_get_vertical_blank_status(_s: *mut c_void, b: *mut u32) -> Hr {
    if !b.is_null() { *b = 1; }
    DD_OK
}
unsafe extern "system" fn ed_initialize(_s: *mut c_void, _d: *mut c_void) -> Hr { DDERR_ALREADYINITIALIZED }
unsafe extern "system" fn ed_restore_display_mode(_s: *mut c_void) -> Hr { DD_OK }
unsafe extern "system" fn ed_restore_all_surfaces(_s: *mut c_void) -> Hr { DD_OK }
unsafe extern "system" fn ed_test_cooperative_level(_s: *mut c_void) -> Hr { DD_OK }

unsafe extern "system" fn ed_set_cooperative_level(_s: *mut c_void, hwnd: *mut c_void, flags: u32) -> Hr {
    let e = emu();
    e.hwnd = hwnd as isize;
    e.fs = flags & 1 != 0;
    if let Some(cb) = e.coop_hook {
        cb(hwnd as isize);
    }
    DD_OK
}

unsafe extern "system" fn ed_set_display_mode(_s: *mut c_void, w: u32, h: u32, _bpp: u32, _r: u32, _f: u32) -> Hr {
    let e = emu();
    e.disp_w = w as i32;
    e.disp_h = h as i32;
    if let Some(p) = e.primary {
        (*p).w = w as i32;
        (*p).h = h as i32;
        (*p).dirty = true;
        (*p).dpart = false;
    }
    if let Some(b) = e.back3d {
        (*b).w = w as i32;
        (*b).h = h as i32;
        (*b).dirty = true;
        (*b).dpart = false;
    }
    DD_OK
}

unsafe extern "system" fn ed_wait_for_vertical_blank(_s: *mut c_void, _f: u32) -> Hr { DD_OK }
unsafe extern "system" fn ed_get_available_vid_mem(_s: *mut c_void, _c: *mut c_void, total: *mut u32, free: *mut u32) -> Hr {
    if !total.is_null() { *total = 64 * 1024 * 1024; }
    if !free.is_null() { *free = 32 * 1024 * 1024; }
    DD_OK
}
unsafe extern "system" fn ed_get_surface_from_dc(_s: *mut c_void, _h: *mut c_void, _o: *mut c_void) -> Hr { DDERR_NOTFOUND }
unsafe extern "system" fn ed_start_mode_test(_s: *mut c_void, _a: *mut c_void, _f: u32, _t: *mut u32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ed_evaluate_mode(_s: *mut c_void, _r: *mut u32, _i: *mut u32, _t: *mut u32) -> Hr { DDERR_UNSUPPORTED }

unsafe extern "system" fn ed_get_device_identifier(_s: *mut c_void, out: *mut c_void, _f: u32) -> Hr {
    let out = out as *mut u8;
    if out.is_null() { return E_POINTER; }
    core::ptr::write_bytes(out, 0, 1068);
    let name = b"d3d9.dll\0";
    for (i, b) in name.iter().enumerate() {
        *out.add(i) = *b;
    }
    let desc = if emu_ready() && emu().dev.is_some() {
        format!("Direct3D9 (Smd D3D7 emulation on {})\0", crate::win::backend_name())
    } else {
        "Direct3D9 (Smd D3D7 emulation)\0".to_string()
    };
    for (i, b) in desc.bytes().enumerate() {
        *out.add(DI_SZDESCRIPTION + i) = b;
    }
    DD_OK
}

// ---------------------------------------------------------------------------
// IDirect3D7 (8 slots)
// ---------------------------------------------------------------------------

fn d3d7_vt() -> Vec<*mut c_void> {
    slots![e3_qi, e3_addref, e3_release, e3_enum_devices, e3_create_device, e3_create_vertex_buffer, e3_enum_z_buffer_formats, e3_evict_managed_textures]
}

unsafe extern "system" fn e3_qi(_s: *mut c_void, riid: *mut c_void, out: *mut c_void) -> Hr {
    let out = out as *mut *mut c_void;
    if out.is_null() { return E_POINTER; }
    *out = core::ptr::null_mut();
    let e = emu();
    if iid_matches(riid, &IID_UNKNOWN) || iid_matches(riid, &IID_D3D7) {
        singleton_addref(e.obj_d3d7);
        *out = e.obj_d3d7 as *mut c_void;
        return DD_OK;
    }
    if iid_matches(riid, &IID_DD7) {
        singleton_addref(e.obj_dd7);
        *out = e.obj_dd7 as *mut c_void;
        return DD_OK;
    }
    E_NOINTERFACE
}
unsafe extern "system" fn e3_addref(_s: *mut c_void) -> u32 { singleton_addref(emu().obj_d3d7) }
unsafe extern "system" fn e3_release(_s: *mut c_void) -> u32 { singleton_release(emu().obj_d3d7) }
unsafe extern "system" fn e3_enum_devices(_s: *mut c_void, _a: *mut c_void, _b: *mut c_void) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn e3_create_vertex_buffer(_s: *mut c_void, _a: *mut c_void, _b: *mut c_void, _c: *mut c_void) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn e3_enum_z_buffer_formats(_s: *mut c_void, _a: *mut c_void, _b: *mut c_void, _c: *mut c_void) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn e3_evict_managed_textures(_s: *mut c_void) -> Hr { DD_OK }

unsafe extern "system" fn e3_create_device(
    _s: *mut c_void, cls: *mut c_void, surf: *mut c_void, out: *mut c_void,
) -> Hr {
    let out = out as *mut *mut c_void;
    if out.is_null() || surf.is_null() { return DDERR_INVALIDPARAMS; }
    let e = emu();
    let target = surf as *mut SurfObj;
    e.back3d = Some(target);
    if (*target).w > 0 { e.disp_w = (*target).w; }
    if (*target).h > 0 { e.disp_h = (*target).h; }
    if let Some(cb) = e.win_hook {
        cb(e.hwnd);
    }
    if !init_d3d9(e.hwnd, e.interval) {
        return DDERR_GENERIC;
    }
    (*target).caps |= DDSCAPS_3DDEVICE;
    let vts = all();
    e.obj_dev7 = Box::into_raw(Box::new(Obj { vt: vts.dev7, refs: 1 }));
    *out = e.obj_dev7 as *mut c_void;
    DD_OK
}

// ---------------------------------------------------------------------------
// IDirect3DDevice7 (49 slots)
// ---------------------------------------------------------------------------

fn dev7_vt() -> Vec<*mut c_void> {
    slots![
        ev_qi, ev_addref, ev_release, ev_get_caps, ev_enum_texture_formats, ev_begin_scene,
        ev_end_scene, ev_get_direct3d, ev_set_render_target, ev_get_render_target, ev_clear,
        ev_set_transform, ev_get_transform, ev_set_viewport, ev_multiply_transform,
        ev_get_viewport, ev_set_material, ev_get_material, ev_set_light, ev_get_light,
        ev_set_render_state, ev_get_render_state, ev_begin_state_block, ev_end_state_block,
        ev_pre_load, ev_draw_primitive, ev_draw_indexed_primitive, ev_set_clip_status,
        ev_get_clip_status, ev_draw_primitive_strided, ev_draw_indexed_primitive_strided,
        ev_draw_primitive_vb, ev_draw_indexed_primitive_vb, ev_compute_sphere_visibility,
        ev_get_texture, ev_set_texture, ev_get_texture_stage_state, ev_set_texture_stage_state,
        ev_validate_device, ev_apply_state_block, ev_capture_state_block, ev_delete_state_block,
        ev_create_state_block, ev_load, ev_light_enable, ev_get_light_enable, ev_set_clip_plane,
        ev_get_clip_plane, ev_get_info,
    ]
}

unsafe extern "system" fn ev_qi(_s: *mut c_void, riid: *mut c_void, out: *mut c_void) -> Hr {
    let out = out as *mut *mut c_void;
    if out.is_null() { return E_POINTER; }
    *out = core::ptr::null_mut();
    let e = emu();
    if iid_matches(riid, &IID_UNKNOWN) || iid_matches(riid, &IID_DEV7) {
        singleton_addref(e.obj_dev7);
        *out = e.obj_dev7 as *mut c_void;
        return DD_OK;
    }
    E_NOINTERFACE
}
unsafe extern "system" fn ev_addref(_s: *mut c_void) -> u32 { singleton_addref(emu().obj_dev7) }
unsafe extern "system" fn ev_release(_s: *mut c_void) -> u32 { singleton_release(emu().obj_dev7) }
unsafe extern "system" fn ev_get_caps(_s: *mut c_void, _c: *mut c_void) -> Hr { DD_OK }
unsafe extern "system" fn ev_enum_texture_formats(_s: *mut c_void, _cb: *mut c_void, _c: *mut c_void) -> Hr { DD_OK }

unsafe extern "system" fn ev_begin_scene(_s: *mut c_void) -> Hr {
    let e = emu();
    let Some(dev) = e.dev() else { return DD_OK };
    raindrop_d3d9::device::begin_scene(dev);
    e.in_scene = true;
    DD_OK
}

unsafe extern "system" fn ev_end_scene(_s: *mut c_void) -> Hr {
    let e = emu();
    let Some(dev) = e.dev() else { return DD_OK };
    if e.in_scene {
        raindrop_d3d9::device::end_scene(dev);
        e.in_scene = false;
    }
    DD_OK
}

unsafe extern "system" fn ev_get_direct3d(_s: *mut c_void, out: *mut c_void) -> Hr {
    let out = out as *mut *mut c_void;
    let e = emu();
    *out = e.obj_d3d7 as *mut c_void;
    singleton_addref(e.obj_d3d7);
    DD_OK
}

unsafe extern "system" fn ev_set_render_target(_s: *mut c_void, t: *mut c_void, _zb: *mut c_void) -> Hr {
    let e = emu();
    if !t.is_null() {
        e.back3d = Some(t as *mut SurfObj);
    }
    DD_OK
}

unsafe extern "system" fn ev_get_render_target(_s: *mut c_void, out: *mut c_void) -> Hr {
    let out = out as *mut *mut c_void;
    let e = emu();
    match e.back3d {
        Some(b) => {
            *out = b as *mut c_void;
            ev_addref(core::ptr::null_mut());
            DD_OK
        }
        None => DDERR_NOTFOUND,
    }
}

unsafe extern "system" fn ev_clear(
    _s: *mut c_void, n: u32, rects: *const RECT, flags: u32, color: u32, z: f32, stencil: u32,
) -> Hr {
    let e = emu();
    let Some(dev) = e.dev() else { return DD_OK };
    raindrop_d3d9::device::clear(dev, n, rects, flags, color, z, stencil);
    DD_OK
}

unsafe extern "system" fn ev_set_transform(_s: *mut c_void, t: u32, m: *const f32) -> Hr {
    let e = emu();
    let Some(dev) = e.dev() else { return DD_OK };
    raindrop_d3d9::device::set_transform(dev, t, m);
    DD_OK
}
unsafe extern "system" fn ev_get_transform(_s: *mut c_void, t: u32, m: *mut f32) -> Hr {
    let e = emu();
    let Some(dev) = e.dev() else { return DD_OK };
    raindrop_d3d9::device::get_transform(dev, t, m);
    DD_OK
}
unsafe extern "system" fn ev_multiply_transform(_s: *mut c_void, _t: u32, _m: *mut f32) -> Hr { DDERR_UNSUPPORTED }

unsafe extern "system" fn ev_set_viewport(_s: *mut c_void, vp: *const u32) -> Hr {
    let e = emu();
    let Some(dev) = e.dev() else { return DD_OK };
    if vp.is_null() { return DDERR_INVALIDPARAMS; }
    // D3DVIEWPORT7 -> D3DVIEWPORT9 conversion
    let v9 = D3DVIEWPORT9 {
        X: *vp.add(D3DVIEWPORT_X),
        Y: *vp.add(D3DVIEWPORT_X + 1),
        Width: *vp.add(D3DVIEWPORT_W),
        Height: *vp.add(D3DVIEWPORT_H),
        MinZ: f32::from_bits(*vp.add(D3DVIEWPORT_MINZ)),
        MaxZ: f32::from_bits(*vp.add(D3DVIEWPORT_MAXZ)),
    };
    raindrop_d3d9::device::set_viewport(dev, &v9);
    DD_OK
}

unsafe extern "system" fn ev_get_viewport(_s: *mut c_void, vp: *mut u32) -> Hr {
    let e = emu();
    let Some(dev) = e.dev() else { return DD_OK };
    let mut v9 = D3DVIEWPORT9 { X: 0, Y: 0, Width: 0, Height: 0, MinZ: 0.0, MaxZ: 1.0 };
    raindrop_d3d9::device::get_viewport(dev, &mut v9);
    if !vp.is_null() {
        *vp.add(D3DVIEWPORT_X) = v9.X;
        *vp.add(D3DVIEWPORT_X + 1) = v9.Y;
        *vp.add(D3DVIEWPORT_W) = v9.Width;
        *vp.add(D3DVIEWPORT_H) = v9.Height;
        *vp.add(4) = 0.0f32.to_bits();
        *vp.add(5) = f32::from_bits(0x3F800000).to_bits(); // dvMaxX = 1.0
        *vp.add(6) = 0.0f32.to_bits();
        *vp.add(7) = f32::from_bits(0x3F800000).to_bits(); // dvMaxY = 1.0
        *vp.add(D3DVIEWPORT_MINZ) = v9.MinZ.to_bits();
        *vp.add(D3DVIEWPORT_MAXZ) = v9.MaxZ.to_bits();
    }
    DD_OK
}

unsafe extern "system" fn ev_set_material(_s: *mut c_void, _m: *mut c_void) -> Hr { DD_OK }
unsafe extern "system" fn ev_get_material(_s: *mut c_void, _m: *mut c_void) -> Hr { DD_OK }
unsafe extern "system" fn ev_set_light(_s: *mut c_void, _i: u32, _l: *mut c_void) -> Hr { DD_OK }
unsafe extern "system" fn ev_get_light(_s: *mut c_void, _i: u32, _l: *mut c_void) -> Hr { DD_OK }
unsafe extern "system" fn ev_light_enable(_s: *mut c_void, _i: u32, _e: u32) -> Hr { DD_OK }
unsafe extern "system" fn ev_get_light_enable(_s: *mut c_void, _i: u32, _e: *mut u32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ev_set_clip_plane(_s: *mut c_void, _i: u32, _p: *mut f32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ev_get_clip_plane(_s: *mut c_void, _i: u32, _p: *mut f32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ev_set_clip_status(_s: *mut c_void, _c: *mut c_void) -> Hr { DD_OK }
unsafe extern "system" fn ev_get_clip_status(_s: *mut c_void, _c: *mut c_void) -> Hr { DD_OK }
unsafe extern "system" fn ev_pre_load(_s: *mut c_void) -> Hr { DD_OK }
unsafe extern "system" fn ev_validate_device(_s: *mut c_void) -> Hr { DD_OK }
unsafe extern "system" fn ev_begin_state_block(_s: *mut c_void) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ev_end_state_block(_s: *mut c_void, _h: *mut u32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ev_apply_state_block(_s: *mut c_void, _h: u32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ev_capture_state_block(_s: *mut c_void, _h: u32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ev_delete_state_block(_s: *mut c_void, _h: u32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ev_create_state_block(_s: *mut c_void, _h: *mut u32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ev_draw_primitive_vb(_s: *mut c_void, _t: u32, _v: *mut c_void, _st: u32, _f: u32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ev_draw_indexed_primitive_vb(_s: *mut c_void, _t: u32, _v: *mut c_void, _vd: u32, _i: *mut c_void, _ic: u32, _f: u32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ev_draw_primitive_strided(_s: *mut c_void, _t: u32, _d: *mut c_void, _n: u32, _f: u32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ev_draw_indexed_primitive_strided(_s: *mut c_void, _t: u32, _d: *mut c_void, _vc: u32, _i: *mut c_void, _ic: u32, _f: u32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ev_draw_indexed_primitive(_s: *mut c_void, _t: u32, _b: u32, _n: u32, _i: *mut c_void, _ic: u32, _f: u32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ev_compute_sphere_visibility(_s: *mut c_void, _a: *mut c_void, _b: *mut c_void, _c: u32, _d: *mut u32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ev_load(_s: *mut c_void, _a: *mut c_void, _b: *mut c_void, _c: *mut c_void, _d: *mut c_void, _e: u32, _f: u32) -> Hr { DDERR_UNSUPPORTED }
unsafe extern "system" fn ev_get_info(_s: *mut c_void, _a: *mut c_void, _b: *mut u32) -> Hr { DDERR_NOTFOUND }

unsafe extern "system" fn ev_get_texture(_s: *mut c_void, st: u32, out: *mut c_void) -> Hr {
    let out = out as *mut *mut c_void;
    let e = emu();
    if out.is_null() { return E_POINTER; }
    *out = match e.cur_tex.get(st as usize).copied().flatten() {
        Some(t) => t as *mut c_void,
        None => core::ptr::null_mut(),
    };
    DD_OK
}

unsafe extern "system" fn ev_set_texture(_s: *mut c_void, st: u32, t: *mut c_void) -> Hr {
    let e = emu();
    let Some(dev) = e.dev() else { return DD_OK };
    if st >= 8 { return DD_OK; }
    let surf = if t.is_null() { core::ptr::null_mut() } else { t as *mut SurfObj };
    e.cur_tex[st as usize] = if surf.is_null() { None } else { Some(surf) };
    let dtex = if surf.is_null() {
        core::ptr::null_mut()
    } else {
        get_tex(surf).unwrap_or_else(core::ptr::null_mut)
    };
    raindrop_d3d9::device::set_texture(dev, st, dtex);
    DD_OK
}

unsafe extern "system" fn ev_set_render_state(_s: *mut c_void, st: u32, v: u32) -> Hr {
    let e = emu();
    let Some(dev) = e.dev() else { return DD_OK };
    match st {
        17 => {
            e.flush_batch();
            raindrop_d3d9::device::set_sampler_state(dev, 0, D3DSAMP_MAGFILTER, map_filter(v));
        }
        18 => {
            e.flush_batch();
            raindrop_d3d9::device::set_sampler_state(dev, 0, D3DSAMP_MINFILTER, map_filter(v));
        }
        21 => {}
        41 => {
            if e.ck_enable != (v != 0) {
                e.flush_batch();
            }
            e.ck_enable = v != 0;
        }
        _ => {
            if rs_pass(st) {
                e.flush_batch();
                e.rs[st as usize] = v;
                e.rs_set[st as usize] = true;
                raindrop_d3d9::device::set_render_state(dev, st, v);
            }
        }
    }
    DD_OK
}

fn rs_pass(st: u32) -> bool {
    st == 7 || st == 8 || st == 9 || (14..=16).contains(&st) || (19..=20).contains(&st)
        || (22..=29).contains(&st) || (34..=38).contains(&st) || (52..=60).contains(&st)
        || (128..=143).contains(&st)
}

fn map_filter(v: u32) -> u32 {
    match v {
        2 | 4 | 5 | 6 => 2,
        _ => 1,
    }
}

unsafe extern "system" fn ev_get_render_state(_s: *mut c_void, st: u32, out: *mut u32) -> Hr {
    let e = emu();
    if st >= 256 || out.is_null() { return DDERR_INVALIDPARAMS; }
    *out = e.rs[st as usize];
    DD_OK
}

unsafe extern "system" fn ev_set_texture_stage_state(_s: *mut c_void, st: u32, ty: u32, v: u32) -> Hr {
    let e = emu();
    let Some(dev) = e.dev() else { return DD_OK };
    if st >= 8 { return DD_OK; }
    match ty {
        12 => {
            e.flush_batch();
            raindrop_d3d9::device::set_sampler_state(dev, st, D3DSAMP_ADDRESSU, v);
            raindrop_d3d9::device::set_sampler_state(dev, st, D3DSAMP_ADDRESSV, v);
        }
        13 => { e.flush_batch(); raindrop_d3d9::device::set_sampler_state(dev, st, D3DSAMP_ADDRESSU, v); }
        14 => { e.flush_batch(); raindrop_d3d9::device::set_sampler_state(dev, st, D3DSAMP_ADDRESSV, v); }
        15 => { e.flush_batch(); raindrop_d3d9::device::set_sampler_state(dev, st, D3DSAMP_BORDERCOLOR, v); }
        16 => {
            e.flush_batch();
            raindrop_d3d9::device::set_sampler_state(dev, st, D3DSAMP_MAGFILTER,
                if v == 5 { D3DTEXF_ANISOTROPIC } else { map_filter(v) });
        }
        17 => {
            e.flush_batch();
            raindrop_d3d9::device::set_sampler_state(dev, st, D3DSAMP_MINFILTER,
                if v == 3 { D3DTEXF_ANISOTROPIC } else { map_filter(v) });
        }
        18 => { e.flush_batch(); raindrop_d3d9::device::set_sampler_state(dev, st, D3DSAMP_MIPFILTER, if v > 0 { v - 1 } else { 0 }); }
        19 => { e.flush_batch(); raindrop_d3d9::device::set_sampler_state(dev, st, D3DSAMP_MIPMAPLODBIAS, v); }
        20 => { e.flush_batch(); raindrop_d3d9::device::set_sampler_state(dev, st, D3DSAMP_MAXMIPLEVEL, v); }
        21 => { e.flush_batch(); raindrop_d3d9::device::set_sampler_state(dev, st, D3DSAMP_MAXANISOTROPY, v); }
        _ => {
            if (1..32).contains(&ty) {
                e.flush_batch();
                e.tss[st as usize][ty as usize] = v;
                e.tss_set[st as usize][ty as usize] = true;
                raindrop_d3d9::device::set_texture_stage_state(dev, st, ty, v);
            }
        }
    }
    DD_OK
}

unsafe extern "system" fn ev_get_texture_stage_state(_s: *mut c_void, st: u32, ty: u32, out: *mut u32) -> Hr {
    let e = emu();
    if st >= 8 || ty >= 32 || out.is_null() { return DDERR_INVALIDPARAMS; }
    *out = e.tss[st as usize][ty as usize];
    DD_OK
}

unsafe extern "system" fn ev_draw_primitive(
    _s: *mut c_void, pt: u32, fvf: u32, verts: *mut c_void, count: u32, _flags: u32,
) -> Hr {
    let e = emu();
    let Some(dev) = e.dev() else { return DD_OK };
    if verts.is_null() || count == 0 { return DD_OK; }
    let stride = fvf_stride(fvf);
    let prims = match pt {
        1 => count,
        2 => count / 2,
        3 => if count > 1 { count - 1 } else { 0 },
        4 => count / 3,
        5 | 6 => if count > 2 { count - 2 } else { 0 },
        _ => 0,
    };
    if stride == 0 || prims == 0 { return DD_OK; }
    let t0 = e.cur_tex[0];
    if let Some(t) = t0 {
        if (*t).dirty {
            get_tex(t);
        }
    }
    let ck = e.ck_enable && t0.map(|t| (*t).has_key).unwrap_or(false);
    if ck {
        // save + apply color-key alpha test state (C-parity D7_CkBegin)
        let at = if e.rs_set[D3DRS_ALPHATESTENABLE as usize] { e.rs[D3DRS_ALPHATESTENABLE as usize] } else { 0 };
        let af = if e.rs_set[D3DRS_ALPHAFUNC as usize] { e.rs[D3DRS_ALPHAFUNC as usize] } else { D3DCMP_ALWAYS };
        let ar = if e.rs_set[D3DRS_ALPHAREF as usize] { e.rs[D3DRS_ALPHAREF as usize] } else { 0 };
        let (op, a1, a2) = (
            if e.tss_set[0][D3DTSS_ALPHAOP as usize] { e.tss[0][D3DTSS_ALPHAOP as usize] } else { D3DTOP_SELECTARG1 },
            if e.tss_set[0][D3DTSS_ALPHAARG1 as usize] { e.tss[0][D3DTSS_ALPHAARG1 as usize] } else { D3DTA_TEXTURE },
            if e.tss_set[0][D3DTSS_ALPHAARG2 as usize] { e.tss[0][D3DTSS_ALPHAARG2 as usize] } else { D3DTA_CURRENT },
        );
        e.flush_batch();
        raindrop_d3d9::device::set_render_state(dev, D3DRS_ALPHATESTENABLE, 1);
        raindrop_d3d9::device::set_render_state(dev, D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        raindrop_d3d9::device::set_render_state(dev, D3DRS_ALPHAREF, 0);
        raindrop_d3d9::device::set_texture_stage_state(dev, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        raindrop_d3d9::device::set_texture_stage_state(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        raindrop_d3d9::device::set_texture_stage_state(dev, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        raindrop_d3d9::device::set_fvf(dev, fvf);
        let hr = raindrop_d3d9::device::draw_primitive_up(dev, pt, prims, verts, stride);
        // restore
        raindrop_d3d9::device::set_render_state(dev, D3DRS_ALPHATESTENABLE, at);
        raindrop_d3d9::device::set_render_state(dev, D3DRS_ALPHAFUNC, af);
        raindrop_d3d9::device::set_render_state(dev, D3DRS_ALPHAREF, ar);
        raindrop_d3d9::device::set_texture_stage_state(dev, 0, D3DTSS_ALPHAOP, op);
        raindrop_d3d9::device::set_texture_stage_state(dev, 0, D3DTSS_ALPHAARG1, a1);
        raindrop_d3d9::device::set_texture_stage_state(dev, 0, D3DTSS_ALPHAARG2, a2);
        return hr;
    }
    e.flush_batch();
    raindrop_d3d9::device::set_fvf(dev, fvf);
    raindrop_d3d9::device::draw_primitive_up(dev, pt, prims, verts, stride)
}

pub fn fvf_stride(fvf: u32) -> u32 {
    let mut sz = 0u32;
    if fvf & D3DFVF_XYZRHW != 0 { sz += 16; }
    else if fvf & D3DFVF_XYZ != 0 { sz += 12; }
    let n = (fvf >> 8) & 0xF;
    sz += 8 * n;
    if fvf & D3DFVF_DIFFUSE != 0 { sz += 4; }
    if fvf & D3DFVF_SPECULAR != 0 { sz += 4; }
    sz
}

// ---------------------------------------------------------------------------
// present plumbing
// ---------------------------------------------------------------------------

unsafe fn present_now() -> Hr {
    let e = emu();
    let Some(dev) = e.dev() else { return DD_OK };
    e.flush_batch();
    let hr = raindrop_d3d9::device::present(dev, core::ptr::null(), core::ptr::null(), e.hwnd as *mut c_void, core::ptr::null());
    recover_if_lost(dev, hr);
    hr
}

unsafe fn recover_if_lost(dev: *mut raindrop_d3d9::com::ComObj, hr: Hr) {
    if hr == D3DERR_DEVICELOST || hr == D3DERR_DEVICENOTRESET {
        let tcl = raindrop_d3d9::device::test_cooperative_level(dev);
        if tcl == D3DERR_DEVICENOTRESET {
            let d = &*((*dev).data as *mut raindrop_d3d9::device::Device);
            let mut pp = d.params;
            let hr2 = raindrop_d3d9::device::reset(dev, &mut pp);
            if hr2 >= 0 {
                crate::win::log("D3D7 emu: device reset");
                reapply_state(dev);
            }
        }
    }
}

unsafe fn reapply_state(dev: *mut raindrop_d3d9::com::ComObj) {
    let d = &*((*dev).data as *mut raindrop_d3d9::device::Device);
    for i in 0..256usize {
        if d.rs[i] != 0 {
            raindrop_d3d9::device::set_render_state(dev, i as u32, d.rs[i]);
        }
    }
    for s in 0..8usize {
        for t in 0..33usize {
            if d.tss[s][t] != 0 {
                raindrop_d3d9::device::set_texture_stage_state(dev, s as u32, t as u32, d.tss[s][t]);
            }
        }
        for m in 0..16usize {
            if d.smp[s][m] != 0 {
                raindrop_d3d9::device::set_sampler_state(dev, s as u32, m as u32, d.smp[s][m]);
            }
        }
    }
}

// ---- DDCAPS bits ----------------------------------------------------------
pub const DDCAPS_3D: u32 = 1;
pub const DDCAPS_BLT: u32 = 64;
pub const DDCAPS_BLTCOLORFILL: u32 = 67108864;
pub const DDCAPS_BLTSTRETCH: u32 = 512;
pub const DDCAPS_GDI: u32 = 1024;
pub const DDCAPS_CANBLTSYSMEM: u32 = 2147483648;
pub const DDLOCK_READONLY: u32 = 16;
pub const DDSCAPS_VIDEOMEMORY: u32 = 16384;

fn opt_rect(s: *mut SurfObj, r: *const RECT) -> Option<Rect> {
    unsafe {
        if r.is_null() {
            return None;
        }
        let s = &*s;
        let r = &*r;
        if r.right <= r.left || r.bottom <= r.top || r.left < 0 || r.top < 0 || r.right > s.w || r.bottom > s.h {
            return None;
        }
        Some(Rect { left: r.left, top: r.top, right: r.right, bottom: r.bottom })
    }
}
