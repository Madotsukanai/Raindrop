// Global emulation state (single-threaded, game main thread — same assumption
// as the C D7 emu it replaces).

use crate::com::*;
use crate::surf::SurfObj;

pub const D3D_SDK_VERSION: u32 = 31;

pub struct Emu {
    pub active: bool,
    pub hwnd: isize,
    pub disp_w: i32,
    pub disp_h: i32,
    pub d3d9: Option<*mut raindrop_d3d9::com::ComObj>,
    pub dev: Option<*mut raindrop_d3d9::com::ComObj>,
    pub interval: u32,
    pub primary: Option<*mut SurfObj>,
    pub back3d: Option<*mut SurfObj>,
    pub cur_tex: [Option<*mut SurfObj>; 8],
    pub rs: [u32; 256],
    pub rs_set: [bool; 256],
    pub tss: [[u32; 32]; 8],
    pub tss_set: [[bool; 32]; 8],
    pub ck_enable: bool,
    pub fvf: u32,
    pub in_scene: bool,
    pub fs: bool,
    /// C-side hooks registered by the shim (mod-specific behavior stays in C)
    pub coop_hook: Option<extern "system" fn(isize)>,
    pub win_hook: Option<extern "system" fn(isize)>,
    pub flush_hook: Option<extern "system" fn()>,
    /// singletons
    pub obj_dd7: *mut Obj,
    pub obj_d3d7: *mut Obj,
    pub obj_dev7: *mut Obj,
}

impl Emu {
    pub fn dev(&self) -> Option<*mut raindrop_d3d9::com::ComObj> {
        self.dev
    }

    /// The C mod batches DrawPrimitive calls; give it a chance to flush before
    /// the emu changes state it batches under (same as the C D7_BatchFlush).
    #[inline]
    pub fn flush_batch(&mut self) {
        if let Some(cb) = self.flush_hook {
            cb();
        }
    }
}

static mut EMU: *mut Emu = core::ptr::null_mut();

pub fn emu() -> &'static mut Emu {
    unsafe { &mut *EMU }
}

pub fn emu_ready() -> bool {
    unsafe { !EMU.is_null() }
}

/// Create the emulation state + the three singleton COM objects.
/// Returns the IDirectDraw7 object pointer (never freed).
pub unsafe fn create(interval: u32, coop_hook: Option<extern "system" fn(isize)>, win_hook: Option<extern "system" fn(isize)>, flush_hook: Option<extern "system" fn()>) -> Option<*mut Obj> {
    if !emu_ready() {
        let e = Box::new(Emu {
            active: true,
            hwnd: 0,
            disp_w: 640,
            disp_h: 480,
            d3d9: None,
            dev: None,
            interval,
            primary: None,
            back3d: None,
            cur_tex: [None; 8],
            rs: [0; 256],
            rs_set: [false; 256],
            tss: [[0; 32]; 8],
            tss_set: [[false; 32]; 8],
            ck_enable: false,
            fvf: 0,
            in_scene: false,
            fs: false,
            coop_hook,
            win_hook,
            flush_hook,
            obj_dd7: core::ptr::null_mut(),
            obj_d3d7: core::ptr::null_mut(),
            obj_dev7: core::ptr::null_mut(),
        });
        EMU = Box::into_raw(e);
        let e = emu();
        // the d3d9 factory is created eagerly; the device is created when the
        // game asks for IDirect3D7::CreateDevice
        let d3d9 = raindrop_d3d9::d9::create_d3d9(D3D_SDK_VERSION);
        e.d3d9 = if d3d9.is_null() { None } else { Some(d3d9) };
        let vts = crate::vtables::all();
        e.obj_dd7 = Box::into_raw(Box::new(Obj { vt: vts.dd7, refs: 1 }));
        e.obj_d3d7 = Box::into_raw(Box::new(Obj { vt: vts.d3d7, refs: 1 }));
        e.obj_dev7 = Box::into_raw(Box::new(Obj { vt: vts.dev7, refs: 1 }));
    }
    let e = emu();
    if e.d3d9.is_none() {
        return None; // wgpu unavailable
    }
    e.active = true;
    singleton_addref(e.obj_dd7);
    Some(e.obj_dd7)
}
