// IDirect3DDevice9 emulation on wgpu: fixed-function 2D subset.
//
// Supported for drawing: XYZRHW (+DIFFUSE, +SPECULAR, +TEX1) via DrawPrimitive /
// DrawIndexedPrimitive / DrawPrimitiveUP / DrawIndexedPrimitiveUP, alpha blending,
// alpha test, textures (A8R8G8B8 / X8R8G8B8 / 565-family) with managed upload.
// Everything else is either stored (states) or logged once and rejected.

use crate::com::*;
use crate::d3d9::*;
use crate::d9::D3D9State;
use crate::render::Renderer;
use crate::types_gen::*;
use crate::vtbl::*;
use core::ffi::c_void;

pub struct Device {
    pub d3d9: *mut ComObj,
    pub behavior: u32,
    pub params: D3DPRESENT_PARAMETERS,
    pub hwnd: isize,
    pub renderer: Renderer,
    /// created resource objects, so Get* can hand them back and cleanup frees them
    pub textures: Vec<*mut ComObj>,
    pub buffers: Vec<*mut ComObj>,
    pub surfaces: Vec<*mut ComObj>,
    pub backbuffer: *mut ComObj,
    pub swapchain: *mut ComObj,
    pub d3d9_obj: *mut ComObj,
    // state
    pub rs: [u32; 256],
    pub tss: [[u32; 33]; 8],
    pub smp: [[u32; 16]; 8],
    pub fvf: u32,
    pub textures_bound: [Option<*mut ComObj>; 8],
    pub stream0: Option<(*mut ComObj, u32, u32)>, // (buffer, offset, stride)
    pub indices: Option<*mut ComObj>,
    pub decl: *mut ComObj,
    pub vp: D3DVIEWPORT9,
    pub transforms: [[f32; 16]; 4],
    pub in_scene: bool,
    pub scene_cleared: bool,
    pub gamma: D3DGAMMARAMP,
    pub svp: i32,
    pub scissor: RECT,
}

impl Device {
    pub fn create(
        state: &mut D3D9State,
        focus: *mut c_void,
        pp: &D3DPRESENT_PARAMETERS,
        behavior: u32,
    ) -> Result<Device, Hr> {
        if focus.is_null() {
            return Err(D3DERR_INVALIDCALL);
        }
        let hwnd = focus as usize as isize;
        let fmt = if bytes_per_pixel(pp.BackBufferFormat) == 0 {
            D3DFMT_X8R8G8B8
        } else {
            pp.BackBufferFormat
        };
        let (w, h) = if pp.BackBufferWidth == 0 || pp.BackBufferHeight == 0 {
            (640u32, 480u32)
        } else {
            (pp.BackBufferWidth, pp.BackBufferHeight)
        };
        if state.ctx.is_none() {
            let c = raindrop_gpu::gpu::init(hwnd).ok_or(D3DERR_NOTAVAILABLE)?;
            state.surface = c.surface;
            state.ctx = Some(std::sync::Arc::new(c.ctx));
        }
        let ctx = state.ctx.clone().unwrap();
        let renderer = Renderer::new(ctx, state.surface.take(), w, h, pp.PresentationInterval)?;
        Ok(Device {
            d3d9: core::ptr::null_mut(),
            behavior,
            params: D3DPRESENT_PARAMETERS { BackBufferWidth: w, BackBufferHeight: h, BackBufferFormat: fmt, ..*pp },
            hwnd,
            renderer,
            textures: Vec::new(),
            buffers: Vec::new(),
            surfaces: Vec::new(),
            backbuffer: core::ptr::null_mut(),
            swapchain: core::ptr::null_mut(),
            d3d9_obj: core::ptr::null_mut(),
            rs: [0; 256],
            tss: [[0; 33]; 8],
            smp: [[0; 16]; 8],
            fvf: 0,
            textures_bound: [None; 8],
            stream0: None,
            indices: None,
            decl: core::ptr::null_mut(),
            vp: D3DVIEWPORT9 { X: 0, Y: 0, Width: w, Height: h, MinZ: 0.0, MaxZ: 1.0 },
            transforms: [identity(); 4],
            in_scene: false,
            scene_cleared: false,
            gamma: id_gamma(),
            svp: 1,
            scissor: RECT { left: 0, top: 0, right: w as i32, bottom: h as i32 },
        })
    }

    pub fn create_backbuffer(&mut self) {
        let vts = vtables();
        let data = Box::into_raw(Box::new(Surface {
            device: core::ptr::null_mut(),
            w: self.params.BackBufferWidth,
            h: self.params.BackBufferHeight,
            fmt: self.params.BackBufferFormat,
            usage: D3DUSAGE_RENDERTARGET,
            pool: D3DPOOL_DEFAULT,
            is_backbuffer: true,
            sys: None,
        })) as *mut c_void;
        self.backbuffer = make_obj(Tag::Surface, vts.3.as_ptr(), data);
        let data2 = Box::into_raw(Box::new(SwapChain { device: self.backbuffer })) as *mut c_void;
        self.swapchain = make_obj(Tag::SwapChain, vts.2.as_ptr(), data2);
    }

    fn win(&self) -> (u32, u32) {
        (self.params.BackBufferWidth, self.params.BackBufferHeight)
    }
}

fn identity() -> [f32; 16] {
    let mut m = [0f32; 16];
    m[0] = 1.0;
    m[5] = 1.0;
    m[10] = 1.0;
    m[15] = 1.0;
    m
}

fn id_gamma() -> D3DGAMMARAMP {
    let mut g = D3DGAMMARAMP { red: [0; 256], green: [0; 256], blue: [0; 256] };
    for i in 0..256 {
        let v = ((i << 8) | i) as u16;
        g.red[i] = v;
        g.green[i] = v;
        g.blue[i] = v;
    }
    g
}

// small "ok with N ignored args" family to keep the vtable type-safe
pub unsafe extern "system" fn ok0(_s: *mut ComObj) -> Hr { S_OK }
pub unsafe extern "system" fn ok1(_s: *mut ComObj, _a: *mut c_void) -> Hr { S_OK }
pub unsafe extern "system" fn ok2(_s: *mut ComObj, _a: *mut c_void, _b: *mut c_void) -> Hr { S_OK }
pub unsafe extern "system" fn ok3(_s: *mut ComObj, _a: u32, _b: u32, _c: *mut c_void) -> Hr { S_OK }
pub unsafe extern "system" fn unimpl2(_s: *mut ComObj, _a: *mut c_void, _b: *mut c_void) -> Hr {
    D3DERR_INVALIDCALL
}
pub unsafe extern "system" fn notavail(_s: *mut ComObj) -> Hr {
    D3DERR_NOTAVAILABLE
}
pub unsafe extern "system" fn na_vol_tex(_s: *mut ComObj) -> Hr { D3DERR_NOTAVAILABLE }
pub unsafe extern "system" fn na_cube_tex(_s: *mut ComObj) -> Hr { D3DERR_NOTAVAILABLE }
pub unsafe extern "system" fn na_front_buf(_s: *mut ComObj, _a: *mut c_void, _b: *mut c_void) -> Hr { D3DERR_NOTAVAILABLE }
pub unsafe extern "system" fn na_clip(_s: *mut ComObj, _a: *mut c_void) -> Hr { D3DERR_NOTAVAILABLE }
pub unsafe extern "system" fn create_vertex_shader_stub(_s: *mut ComObj, _a: *const c_void, _b: *mut c_void) -> Hr { D3DERR_NOTAVAILABLE }
pub unsafe extern "system" fn na_query(_s: *mut ComObj, _a: u32, _b: *mut c_void) -> Hr { D3DERR_NOTAVAILABLE }

unsafe fn dev<'a>(selfp: *mut ComObj) -> &'a mut Device {
    &mut *((*selfp).data as *mut Device)
}

// ---- lifecycle --------------------------------------------------------------

pub unsafe extern "system" fn test_cooperative_level(_s: *mut ComObj) -> Hr {
    S_OK
}

pub unsafe extern "system" fn get_available_texture_mem(_s: *mut ComObj) -> u32 {
    0x4000_0000
}

pub unsafe extern "system" fn evict(_s: *mut ComObj) -> Hr {
    S_OK
}

pub unsafe extern "system" fn get_direct3d(selfp: *mut ComObj, out: *mut *mut c_void) -> Hr {
    let d = dev(selfp);
    unsafe {
        crate::vtbl::addref(d.d3d9_obj);
    }
    *out = d.d3d9_obj as *mut c_void;
    S_OK
}

pub unsafe extern "system" fn get_direct3d_surf(selfp: *mut ComObj, out: *mut *mut c_void) -> Hr {
    // resources carry their owning device in surface->device; the D3D9 object is
    // reached through the device object stored in the resource's device field
    let s = &*(*selfp).data.cast::<Surface>();
    let d = &*((*s.device).data as *mut Device);
    unsafe { crate::vtbl::addref(d.d3d9_obj) };
    *out = d.d3d9_obj as *mut c_void;
    S_OK
}

pub unsafe extern "system" fn get_device_caps(selfp: *mut ComObj, caps: *mut D3DCAPS9) -> Hr {
    let d = dev(selfp);
    let st = &*(*d.d3d9_obj).data.cast::<D3D9State>();
    let fake_d3d9 = d.d3d9_obj;
    let _ = st;
    unsafe { crate::d9::get_device_caps(fake_d3d9, 0, D3DDEVTYPE_HAL, caps) }
}

pub unsafe extern "system" fn get_display_mode(selfp: *mut ComObj, i: u32, mode: *mut D3DDISPLAYMODE) -> Hr {
    let d = dev(selfp);
    if mode.is_null() {
        return D3DERR_INVALIDCALL;
    }
    let (w, h) = d.win();
    *mode = D3DDISPLAYMODE { Width: w, Height: h, RefreshRate: 60, Format: d.params.BackBufferFormat };
    if i != 0 {
        return D3DERR_INVALIDCALL;
    }
    S_OK
}

pub unsafe extern "system" fn get_creation_parameters(selfp: *mut ComObj, out: *mut D3DDEVICE_CREATION_PARAMETERS) -> Hr {
    let d = dev(selfp);
    *out = D3DDEVICE_CREATION_PARAMETERS {
        AdapterOrdinal: 0,
        DeviceType: D3DDEVTYPE_HAL,
        hFocusWindow: d.hwnd as *mut c_void,
        BehaviorFlags: d.behavior,
    };
    S_OK
}

pub unsafe extern "system" fn set_cursor_position(_s: *mut ComObj, _x: u32, _y: u32, _f: u32) -> Hr {
    S_OK
}
pub unsafe extern "system" fn show_cursor(_s: *mut ComObj, _b: i32) -> i32 {
    0
}
pub unsafe extern "system" fn get_raster_status(_s: *mut ComObj, _i: u32, _r: *mut D3DRASTER_STATUS) -> Hr {
    D3DERR_INVALIDCALL
}
pub unsafe extern "system" fn get_gamma_ramp(_s: *mut ComObj, _i: u32, out: *mut D3DGAMMARAMP) {
    let d = unsafe { dev(_s) };
    *out = d.gamma;
}

pub unsafe extern "system" fn reset(selfp: *mut ComObj, pp: *mut D3DPRESENT_PARAMETERS) -> Hr {
    let d = dev(selfp);
    if pp.is_null() {
        return D3DERR_INVALIDCALL;
    }
    let p = unsafe { &*pp };
    let fmt = if bytes_per_pixel(p.BackBufferFormat) == 0 { d.params.BackBufferFormat } else { p.BackBufferFormat };
    let (w, h) = if p.BackBufferWidth == 0 || p.BackBufferHeight == 0 {
        d.win()
    } else {
        (p.BackBufferWidth, p.BackBufferHeight)
    };
    d.params.BackBufferWidth = w;
    d.params.BackBufferHeight = h;
    d.params.BackBufferFormat = fmt;
    d.params.SwapEffect = p.SwapEffect;
    d.params.Windowed = p.Windowed;
    d.params.PresentationInterval = p.PresentationInterval;
    d.vp = D3DVIEWPORT9 { X: 0, Y: 0, Width: w, Height: h, MinZ: 0.0, MaxZ: 1.0 };
    d.renderer.reconfigure(w, h, p.PresentationInterval);
    if let Some(bb) = d.backbuffer.is_null().then_some(0) {
        let _ = bb;
    }
    unsafe {
        if !d.backbuffer.is_null() {
            let s = &mut *((*d.backbuffer).data as *mut Surface);
            s.w = w;
            s.h = h;
        }
    }
    d.in_scene = false;
    S_OK
}

pub unsafe extern "system" fn present(
    selfp: *mut ComObj,
    _src: *const RECT,
    _dst: *const RECT,
    _hwnd: *mut c_void,
    _dirty: *const c_void,
) -> Hr {
    let d = dev(selfp);
    d.renderer.present()
}

pub unsafe extern "system" fn swapchain_present(selfp: *mut ComObj, _a: *const RECT, _b: *const RECT, _c: *mut c_void, _e: *const c_void) -> Hr {
    let sc = &*(*selfp).data.cast::<SwapChain>();
    let d = &mut *((*sc.device).data as *mut Device);
    d.renderer.present()
}

pub unsafe extern "system" fn get_back_buffer(
    selfp: *mut ComObj,
    swapchain: u32,
    idx: u32,
    _t: u32,
    out: *mut *mut c_void,
) -> Hr {
    let d = dev(selfp);
    if swapchain != 0 || idx != 0 {
        return D3DERR_INVALIDCALL;
    }
    unsafe { crate::vtbl::addref(d.backbuffer) };
    *out = d.backbuffer as *mut c_void;
    S_OK
}

// ---- scene ------------------------------------------------------------------

pub unsafe extern "system" fn begin_scene(selfp: *mut ComObj) -> Hr {
    let d = dev(selfp);
    if d.in_scene {
        return D3DERR_INVALIDCALL;
    }
    d.in_scene = true;
    d.scene_cleared = false;
    d.renderer.begin_scene();
    S_OK
}

pub unsafe extern "system" fn end_scene(selfp: *mut ComObj) -> Hr {
    let d = dev(selfp);
    if !d.in_scene {
        return D3DERR_INVALIDCALL;
    }
    d.in_scene = false;
    d.renderer.flush_textures(&d.textures_bound);
    S_OK
}

pub unsafe extern "system" fn clear(
    selfp: *mut ComObj,
    count: u32,
    rects: *const RECT,
    flags: u32,
    color: u32,
    _z: f32,
    _stencil: u32,
) -> Hr {
    let d = dev(selfp);
    if rects.is_null() && count != 0 {
        return D3DERR_INVALIDCALL;
    }
    d.renderer.clear(flags, color);
    d.scene_cleared = true;
    S_OK
}

// ---- resources --------------------------------------------------------------

fn make_sys_level(w: u32, h: u32, fmt: u32) -> SysLevel {
    let bpp = bytes_per_pixel(fmt) as usize;
    SysLevel { pitch: w as usize * bpp, data: vec![0; w as usize * bpp * h as usize], dirty: false }
}

pub unsafe extern "system" fn create_texture(
    selfp: *mut ComObj,
    w: u32,
    h: u32,
    levels: u32,
    usage: u32,
    fmt: u32,
    pool: u32,
    out: *mut *mut c_void,
    _shared: *mut c_void,
) -> Hr {
    let d = dev(selfp);
    if bytes_per_pixel(fmt) == 0 {
        warn_once(Tag::Device, "CreateTexture unsupported format", D3DERR_NOTAVAILABLE);
        return D3DERR_NOTAVAILABLE;
    }
    let levels = if levels == 0 { 1 } else { levels.min(2) };
    let mut sys = Vec::new();
    let mut lw = w;
    let mut lh = h;
    for _ in 0..levels {
        sys.push(make_sys_level(lw, lh, fmt));
        lw = (lw / 2).max(1);
        lh = (lh / 2).max(1);
    }
    let ctx = d.renderer.ctx.clone();
    let gpu = crate::render::create_gpu_texture(&ctx, w, h, fmt, usage);
    let vts = vtables();
    let tex = Texture {
        device: selfp,
        w,
        h,
        levels,
        fmt,
        usage,
        pool,
        levels_sys: sys,
        gpu: gpu.map(|g| g.texture().clone()),
        view: None,
        bg: [None, None],
    };
    let data = Box::into_raw(Box::new(tex)) as *mut c_void;
    let obj = make_obj(Tag::Texture, vts.6.as_ptr(), data);
    d.textures.push(obj);
    *out = obj as *mut c_void;
    S_OK
}

pub unsafe extern "system" fn create_vertex_buffer(
    selfp: *mut ComObj,
    len: u32,
    usage: u32,
    fvf: u32,
    pool: u32,
    out: *mut *mut c_void,
    _shared: *mut c_void,
) -> Hr {
    let d = dev(selfp);
    let vts = vtables();
    let b = Buffer {
        device: selfp,
        len,
        fvf,
        usage,
        pool,
        data: vec![0; len as usize],
        locked: false,
    };
    let data = Box::into_raw(Box::new(b)) as *mut c_void;
    let obj = make_obj(Tag::VertexBuffer, vts.4.as_ptr(), data);
    d.buffers.push(obj);
    *out = obj as *mut c_void;
    S_OK
}

pub unsafe extern "system" fn create_index_buffer(
    selfp: *mut ComObj,
    len: u32,
    usage: u32,
    _fmt: u32,
    pool: u32,
    out: *mut *mut c_void,
    _shared: *mut c_void,
) -> Hr {
    let d = dev(selfp);
    let vts = vtables();
    let b = Buffer {
        device: selfp,
        len,
        fvf: 0,
        usage,
        pool,
        data: vec![0; len as usize],
        locked: false,
    };
    let data = Box::into_raw(Box::new(b)) as *mut c_void;
    let obj = make_obj(Tag::IndexBuffer, vts.5.as_ptr(), data);
    d.buffers.push(obj);
    *out = obj as *mut c_void;
    S_OK
}

fn make_surface_obj(devp: *mut ComObj, w: u32, h: u32, fmt: u32, usage: u32, pool: u32, lockable: bool, is_bb: bool) -> *mut ComObj {
    let vts = vtables();
    let s = Surface {
        device: devp,
        w,
        h,
        fmt,
        usage,
        pool,
        is_backbuffer: is_bb,
        sys: if lockable && !is_bb { Some(make_sys_level(w, h, fmt)) } else { None },
    };
    let data = Box::into_raw(Box::new(s)) as *mut c_void;
    make_obj(Tag::Surface, vts.3.as_ptr(), data)
}

pub unsafe extern "system" fn create_render_target(
    selfp: *mut ComObj,
    w: u32,
    h: u32,
    fmt: u32,
    ms: u32,
    _msq: u32,
    lockable: i32,
    out: *mut *mut c_void,
    _shared: *mut c_void,
) -> Hr {
    let d = dev(selfp);
    if ms != 0 || bytes_per_pixel(fmt) == 0 {
        return D3DERR_INVALIDCALL;
    }
    let obj = make_surface_obj(selfp, w, h, fmt, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, lockable != 0, false);
    d.surfaces.push(obj);
    *out = obj as *mut c_void;
    S_OK
}

pub unsafe extern "system" fn create_depth_stencil(
    selfp: *mut ComObj,
    _w: u32,
    _h: u32,
    _fmt: u32,
    ms: u32,
    _msq: u32,
    _discard: i32,
    out: *mut *mut c_void,
    _shared: *mut c_void,
) -> Hr {
    let d = dev(selfp);
    if ms != 0 {
        return D3DERR_INVALIDCALL;
    }
    // depth attachment is not emulated; hand back a dummy surface
    let obj = make_surface_obj(selfp, 1, 1, D3DFMT_D16, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, false, false);
    d.surfaces.push(obj);
    *out = obj as *mut c_void;
    S_OK
}

pub unsafe extern "system" fn create_offscreen_surface(
    selfp: *mut ComObj,
    w: u32,
    h: u32,
    fmt: u32,
    pool: u32,
    out: *mut *mut c_void,
    _shared: *mut c_void,
) -> Hr {
    let d = dev(selfp);    if bytes_per_pixel(fmt) == 0 {
        return D3DERR_NOTAVAILABLE;
    }
    let obj = make_surface_obj(selfp, w, h, fmt, 0, pool, true, false);
    d.surfaces.push(obj);
    *out = obj as *mut c_void;
    S_OK
}

pub unsafe extern "system" fn update_surface(
    selfp: *mut ComObj,
    src: *mut ComObj,
    _srcrect: *const RECT,
    dst: *mut ComObj,
    _dstpoint: *const crate::types_gen::D3DVIEWPORT9,
) -> Hr {
    let d = dev(selfp);
    if src.is_null() || dst.is_null() {
        return D3DERR_INVALIDCALL;
    }
    let s = &*((*src).data as *mut Surface);
    let t = &mut *((*dst).data as *mut Surface);
    if let (Some(sd), Some(td)) = (&s.sys, &mut t.sys) {
        let n = td.data.len().min(sd.data.len());
        td.data[..n].copy_from_slice(&sd.data[..n]);
        td.dirty = true;
        let ctx = d.renderer.ctx.clone();
        crate::render::upload_surface(&ctx, dst, t);
    }
    S_OK
}

pub unsafe extern "system" fn update_texture(
    selfp: *mut ComObj,
    src: *mut ComObj,
    dst: *mut ComObj,
) -> Hr {
    let d = dev(selfp);
    if src.is_null() || dst.is_null() {
        return D3DERR_INVALIDCALL;
    }
    let s = &*((*src).data as *mut Texture);
    let t = &mut *((*dst).data as *mut Texture);
    if s.fmt != t.fmt {
        return D3DERR_INVALIDCALL;
    }
    for (i, sl) in s.levels_sys.iter().enumerate() {
        if let Some(tl) = t.levels_sys.get_mut(i) {
            let n = tl.data.len().min(sl.data.len());
            tl.data[..n].copy_from_slice(&sl.data[..n]);
            tl.dirty = true;
        }
    }
    let ctx = d.renderer.ctx.clone();
    crate::render::upload_texture(&ctx, dst, t);
    S_OK
}

pub unsafe extern "system" fn stretch_rect(
    selfp: *mut ComObj,
    src: *mut ComObj,
    _srcrect: *const RECT,
    dst: *mut ComObj,
    _dstrect: *const RECT,
    _filter: u32,
) -> Hr {
    let d = dev(selfp);
    if src.is_null() || dst.is_null() {
        return D3DERR_INVALIDCALL;
    }
    warn_once(Tag::Device, "StretchRect (no-op copy)", D3DERR_INVALIDCALL);
    let _ = d;
    S_OK
}

pub unsafe extern "system" fn color_fill(
    selfp: *mut ComObj,
    dst: *mut ComObj,
    _rect: *const RECT,
    color: u32,
) -> Hr {
    let d = dev(selfp);
    if dst.is_null() {
        return D3DERR_INVALIDCALL;
    }
    let t = &mut *((*dst).data as *mut Surface);
    if let Some(sys) = &mut t.sys {
        let bpp = bytes_per_pixel(t.fmt) as usize;
        let c = color.to_le_bytes();
        for row in sys.data.chunks_mut(sys.pitch) {
            for px in row.chunks_mut(bpp) {
                px.copy_from_slice(&c[..px.len()]);
            }
        }
        sys.dirty = true;
        let ctx = d.renderer.ctx.clone();
        crate::render::upload_surface(&ctx, dst, t);
    }
    S_OK
}

// ---- render target ----------------------------------------------------------

pub unsafe extern "system" fn set_render_target(selfp: *mut ComObj, idx: u32, rt: *mut ComObj) -> Hr {
    let d = dev(selfp);
    if idx != 0 {
        return D3DERR_INVALIDCALL;
    }
    if rt.is_null() {
        // NULL == backbuffer
        return S_OK;
    }
    let s = &*((*rt).data as *mut Surface);
    if !s.is_backbuffer {
        warn_once(Tag::Device, "SetRenderTarget(non-backbuffer target)", D3DERR_INVALIDCALL);
        return D3DERR_INVALIDCALL;
    }
    S_OK
}

pub unsafe extern "system" fn get_render_target(selfp: *mut ComObj, idx: u32, out: *mut *mut c_void) -> Hr {
    let d = dev(selfp);
    if idx != 0 {
        return D3DERR_INVALIDCALL;
    }
    unsafe { crate::vtbl::addref(d.backbuffer) };
    *out = d.backbuffer as *mut c_void;
    S_OK
}

pub unsafe extern "system" fn set_depth_stencil(_self: *mut ComObj, _ds: *mut ComObj) -> Hr {
    // depth is not emulated; accept and ignore
    S_OK
}

pub unsafe extern "system" fn get_depth_stencil(_self: *mut ComObj, out: *mut *mut c_void) -> Hr {
    *out = core::ptr::null_mut();
    S_OK
}

// ---- state ------------------------------------------------------------------

pub unsafe extern "system" fn set_transform(selfp: *mut ComObj, which: u32, m: *const f32) -> Hr {
    let d = dev(selfp);
    if m.is_null() {
        return D3DERR_INVALIDCALL;
    }
    let idx = match which {
        D3DTS_WORLD => 0,
        D3DTS_VIEW => 1,
        D3DTS_PROJECTION => 2,
        x if x > 255 => 3,
        _ => return D3DERR_INVALIDCALL,
    };
    unsafe { core::ptr::copy_nonoverlapping(m, d.transforms[idx].as_mut_ptr(), 16) };
    S_OK
}

pub unsafe extern "system" fn get_transform(selfp: *mut ComObj, which: u32, out: *mut f32) -> Hr {
    let d = dev(selfp);
    if out.is_null() {
        return D3DERR_INVALIDCALL;
    }
    let idx = match which {
        D3DTS_WORLD => 0,
        D3DTS_VIEW => 1,
        D3DTS_PROJECTION => 2,
        x if x > 255 => 3,
        _ => return D3DERR_INVALIDCALL,
    };
    unsafe { core::ptr::copy_nonoverlapping(d.transforms[idx].as_ptr(), out, 16) };
    S_OK
}

pub unsafe extern "system" fn set_viewport(selfp: *mut ComObj, vp: *const D3DVIEWPORT9) -> Hr {
    let d = dev(selfp);
    if vp.is_null() {
        return D3DERR_INVALIDCALL;
    }
    d.vp = unsafe { *vp };
    d.renderer.set_viewport(&d.vp);
    S_OK
}

pub unsafe extern "system" fn get_viewport(selfp: *mut ComObj, out: *mut D3DVIEWPORT9) -> Hr {
    let d = dev(selfp);
    if out.is_null() {
        return D3DERR_INVALIDCALL;
    }
    *out = d.vp;
    S_OK
}

pub unsafe extern "system" fn set_render_state(selfp: *mut ComObj, state: u32, value: u32) -> Hr {
    let d = dev(selfp);
    if state >= 256 {
        return D3DERR_INVALIDCALL;
    }
    d.rs[state as usize] = value;
    d.renderer.set_rs(state, value);
    S_OK
}

pub unsafe extern "system" fn get_render_state(selfp: *mut ComObj, state: u32, out: *mut u32) -> Hr {
    let d = dev(selfp);
    if state >= 256 || out.is_null() {
        return D3DERR_INVALIDCALL;
    }
    *out = d.rs[state as usize];
    S_OK
}

pub unsafe extern "system" fn get_texture(selfp: *mut ComObj, stage: u32, out: *mut *mut c_void) -> Hr {
    let d = dev(selfp);
    if stage >= 8 {
        return D3DERR_INVALIDCALL;
    }
    match d.textures_bound[stage as usize] {
        Some(t) => {
            let _ = unsafe { crate::vtbl::addref(t) };
        }
        None => {}
    }
    *out = d.textures_bound[stage as usize].unwrap_or(core::ptr::null_mut()) as *mut c_void;
    S_OK
}

pub unsafe extern "system" fn set_texture(selfp: *mut ComObj, stage: u32, tex: *mut ComObj) -> Hr {
    let d = dev(selfp);
    if stage >= 8 {
        return D3DERR_INVALIDCALL;
    }
    d.textures_bound[stage as usize] = if tex.is_null() { None } else { Some(tex) };
    S_OK
}

pub unsafe extern "system" fn get_texture_stage_state(selfp: *mut ComObj, stage: u32, tss: u32, out: *mut u32) -> Hr {
    let d = dev(selfp);
    if stage >= 8 || tss >= 33 || out.is_null() {
        return D3DERR_INVALIDCALL;
    }
    *out = d.tss[stage as usize][tss as usize];
    S_OK
}

pub unsafe extern "system" fn set_texture_stage_state(selfp: *mut ComObj, stage: u32, tss: u32, value: u32) -> Hr {
    let d = dev(selfp);
    if stage >= 8 || tss >= 33 {
        return D3DERR_INVALIDCALL;
    }
    d.tss[stage as usize][tss as usize] = value;
    S_OK
}

pub unsafe extern "system" fn get_sampler_state(selfp: *mut ComObj, stage: u32, s: u32, out: *mut u32) -> Hr {
    let d = dev(selfp);
    if stage >= 8 || s >= 16 || out.is_null() {
        return D3DERR_INVALIDCALL;
    }
    *out = d.smp[stage as usize][s as usize];
    S_OK
}

pub unsafe extern "system" fn set_sampler_state(selfp: *mut ComObj, stage: u32, s: u32, value: u32) -> Hr {
    let d = dev(selfp);
    if stage >= 8 || s >= 16 {
        return D3DERR_INVALIDCALL;
    }
    d.smp[stage as usize][s as usize] = value;
    S_OK
}

pub unsafe extern "system" fn set_svp(selfp: *mut ComObj, v: i32) -> Hr {
    let d = dev(selfp);
    d.svp = v;
    S_OK
}
pub unsafe extern "system" fn get_svp(selfp: *mut ComObj) -> i32 {
    dev(selfp).svp
}

pub unsafe extern "system" fn set_fvf(selfp: *mut ComObj, fvf: u32) -> Hr {
    let d = dev(selfp);
    d.fvf = fvf;
    S_OK
}
/// D3DVERTEXELEMENT9 is 8 bytes: WORD Stream, WORD Offset, BYTE Type, BYTE Method, BYTE Usage, BYTE UsageIndex
const DECL_END: u8 = 0xFF;

/// Convert a vertex declaration to the equivalent FVF (best effort for our FVF-based renderer).
fn decl_to_fvf(decl: &[u8]) -> u32 {
    let n = decl.len() / 8;
    let mut fvf = 0u32;
    let mut rhw = false;
    let mut xyz = false;
    let mut color = false;
    let mut tex = false;
    let mut texn = 0u32;
    for i in 0..n {
        let e = &decl[i * 8..];
        let usage = e[5];
        let typ = e[4];
        if e[2] == DECL_END {
            break;
        }
        match usage {
            0 => {
                if typ == 3 { rhw = true; } // D3DDECLTYPE_FLOAT4
                else if typ == 2 { xyz = true; }
            }
            9 => rhw = true, // D3DDECLUSAGE_POSITIONT
            10 => color = true, // D3DDECLUSAGE_COLOR
            5 => {
                if e[6] == 0 { tex = true; texn += 1; } // first texcoord stage
            }
            _ => {}
        }
    }
    if rhw {
        fvf |= 4; // D3DFVF_XYZRHW
    } else if xyz {
        fvf |= 2; // D3DFVF_XYZ
    } else {
        fvf |= 4;
    }
    if color {
        fvf |= 0x40; // D3DFVF_DIFFUSE
    }
    if tex {
        fvf |= 0x100 | ((texn - 1) << 8).min(0x000); // D3DFVF_TEX1
    }
    fvf
}

pub unsafe extern "system" fn create_vertex_declaration(
    selfp: *mut ComObj,
    elements: *const u8,
    out: *mut *mut c_void,
) -> Hr {
    if elements.is_null() || out.is_null() {
        return D3DERR_INVALIDCALL;
    }
    // count until D3DDECL_END
    let mut n = 0usize;
    while n < 32 {
        let e = core::slice::from_raw_parts(elements.add(n * 8), 8);
        if e[2] == DECL_END {
            n += 1;
            break;
        }
        n += 1;
    }
    if n == 0 {
        return D3DERR_INVALIDCALL;
    }
    let bytes = core::slice::from_raw_parts(elements, n * 8).to_vec();
    let data = Box::into_raw(Box::new(bytes)) as *mut c_void;
    let vts = vtables();
    let obj = make_obj(Tag::VertexDeclaration, vts.7.as_ptr(), data);
    *out = obj as *mut c_void;
    S_OK
}

pub unsafe extern "system" fn set_vertex_declaration(selfp: *mut ComObj, decl: *mut ComObj) -> Hr {
    let d = dev(selfp);
    d.decl = decl;
    if !decl.is_null() {
        let bytes = &*((*decl).data as *const Vec<u8>);
        let fvf = decl_to_fvf(bytes);
        if fvf != 0 && d.fvf == 0 {
            d.fvf = fvf;
        }
    }
    S_OK
}

pub unsafe extern "system" fn get_vertex_declaration(selfp: *mut ComObj, out: *mut *mut c_void) -> Hr {
    let d = dev(selfp);
    let p = d.decl;
    if !p.is_null() {
        addref(p);
    }
    *out = p as *mut c_void;
    S_OK
}

pub unsafe extern "system" fn get_fvf(selfp: *mut ComObj, out: *mut u32) -> Hr {
    let d = dev(selfp);
    *out = d.fvf;
    S_OK
}

pub unsafe extern "system" fn set_vertex_shader(selfp: *mut ComObj, shader: *mut ComObj) -> Hr {
    if !shader.is_null() {
        warn_once(Tag::Device, "SetVertexShader (custom shader)", D3DERR_INVALIDCALL);
    }
    S_OK
}
pub unsafe extern "system" fn set_pixel_shader(selfp: *mut ComObj, shader: *mut ComObj) -> Hr {
    if !shader.is_null() {
        warn_once(Tag::Device, "SetPixelShader (custom shader)", D3DERR_INVALIDCALL);
    }
    S_OK
}
pub unsafe extern "system" fn set_vsc_f(_s: *mut ComObj, _i: u32, _n: u32, _d: *const f32) -> Hr {
    S_OK
}
pub unsafe extern "system" fn set_psc_f(_s: *mut ComObj, _i: u32, _n: u32, _d: *const f32) -> Hr {
    S_OK
}

pub unsafe extern "system" fn set_stream_source(selfp: *mut ComObj, n: u32, vb: *mut ComObj, off: u32, stride: u32) -> Hr {
    let d = dev(selfp);
    if n != 0 {
        return D3DERR_INVALIDCALL;
    }
    d.stream0 = if vb.is_null() { None } else { Some((vb, off, stride)) };
    S_OK
}

pub unsafe extern "system" fn set_indices(selfp: *mut ComObj, ib: *mut ComObj) -> Hr {
    let d = dev(selfp);
    d.indices = if ib.is_null() { None } else { Some(ib) };
    S_OK
}

// ---- drawing ----------------------------------------------------------------

pub fn fvf_stride(fvf: u32) -> u32 {
    let mut s = 0u32;
    match fvf & D3DFVF_POSITION_MASK {
        D3DFVF_XYZ => s += 12,
        D3DFVF_XYZRHW => s += 16,
        _ => return 0,
    }
    if fvf & D3DFVF_DIFFUSE != 0 {
        s += 4;
    }
    if fvf & D3DFVF_SPECULAR != 0 {
        s += 4;
    }
    let n = (fvf >> 8) & 0xF;
    if n > 0 {
        s += 8 * n.min(8);
    }
    s
}

pub unsafe extern "system" fn draw_primitive(selfp: *mut ComObj, prim: u32, start: u32, count: u32) -> Hr {
    let d = dev(selfp);
    let Some((vb, off, stride)) = d.stream0 else {
        warn_once(Tag::Device, "DrawPrimitive without stream", D3DERR_INVALIDCALL);
        return D3DERR_INVALIDCALL;
    };
    if stride == 0 {
        return D3DERR_INVALIDCALL;
    }
    let b = &*((*vb).data as *mut Buffer);
    let start = start as usize * stride as usize + off as usize;
    let nverts = prim_vertex_count(prim, count) as usize;
    if start + nverts * stride as usize > b.data.len() {
        return D3DERR_INVALIDCALL;
    }
    let verts = &b.data[start..start + nverts * stride as usize];
    d.renderer.draw(
        prim,
        d.fvf,
        verts,
        stride,
        nverts,
        core::ptr::null(),
        0,
        &d.textures_bound,
        &d.rs,
        &d.tss,
        &d.smp,
        &d.vp,
    )
}

pub unsafe extern "system" fn draw_indexed_primitive(
    selfp: *mut ComObj,
    prim: u32,
    base_vertex: i32,
    _min_index: u32,
    num_vertices: u32,
    start_index: u32,
    prim_count: u32,
) -> Hr {
    let d = dev(selfp);
    let Some((vb, off, stride)) = d.stream0 else {
        return D3DERR_INVALIDCALL;
    };
    if stride == 0 {
        return D3DERR_INVALIDCALL;
    }
    let Some(ib) = d.indices else {
        return D3DERR_INVALIDCALL;
    };
    let b = &*((*vb).data as *mut Buffer);
    let idx = &*((*ib).data as *mut Buffer);
    let nidx = prim_vertex_count(prim, prim_count) as usize;
    let si = start_index as usize * 2;
    if si + nidx * 2 > idx.data.len() {
        return D3DERR_INVALIDCALL;
    }
    // de-index into a temp vertex list (small counts, keeps one draw path)
    let indices = unsafe { core::slice::from_raw_parts(idx.data.as_ptr().add(si) as *const u16, nidx) };
    let mut verts = Vec::with_capacity(nidx * stride as usize);
    let mut ok = true;
    for &i in indices {
        let v = i as i64 + base_vertex as i64;
        let start = off as usize + v as usize * stride as usize;
        if start + stride as usize > b.data.len() {
            ok = false;
            break;
        }
        verts.extend_from_slice(&b.data[start..start + stride as usize]);
    }
    if !ok {
        return D3DERR_INVALIDCALL;
    }
    d.renderer.draw(
        prim,
        d.fvf,
        &verts,
        stride,
        nidx,
        core::ptr::null(),
        0,
        &d.textures_bound,
        &d.rs,
        &d.tss,
        &d.smp,
        &d.vp,
    )
}

pub unsafe extern "system" fn draw_primitive_up(
    selfp: *mut ComObj,
    prim: u32,
    count: u32,
    data: *const c_void,
    stride: u32,
) -> Hr {
    let d = dev(selfp);
    if data.is_null() || stride == 0 {
        return D3DERR_INVALIDCALL;
    }
    let nverts = prim_vertex_count(prim, count) as usize;
    let verts = unsafe { core::slice::from_raw_parts(data as *const u8, nverts * stride as usize) };
    d.renderer.flush_textures(&d.textures_bound);
    d.renderer.draw(
        prim,
        d.fvf,
        verts,
        stride,
        nverts,
        core::ptr::null(),
        0,
        &d.textures_bound,
        &d.rs,
        &d.tss,
        &d.smp,
        &d.vp,
    )
}

pub unsafe extern "system" fn draw_indexed_primitive_up(
    selfp: *mut ComObj,
    prim: u32,
    _minv: u32,
    _numv: u32,
    icount: u32,
    idxdata: *const c_void,
    _idxfmt: u32,
    vtxdata: *const c_void,
    stride: u32,
) -> Hr {
    let d = dev(selfp);
    if idxdata.is_null() || vtxdata.is_null() || stride == 0 {
        return D3DERR_INVALIDCALL;
    }
    let nidx = prim_vertex_count(prim, icount) as usize;
    let indices = unsafe { core::slice::from_raw_parts(idxdata as *const u16, nidx) };
    let nverts = (indices.iter().copied().max().unwrap_or(0) as usize + 1) * stride as usize;
    let verts = unsafe { core::slice::from_raw_parts(vtxdata as *const u8, nverts) };
    d.renderer.draw_indexed(
        prim,
        d.fvf,
        verts,
        stride,
        indices,
        &d.textures_bound,
        &d.rs,
        &d.tss,
        &d.smp,
        &d.vp,
    )
}

fn prim_vertex_count(prim: u32, count: u32) -> u32 {
    match prim {
        D3DPT_POINTLIST => count,
        D3DPT_LINELIST => count * 2,
        D3DPT_LINESTRIP => count + 1,
        D3DPT_TRIANGLELIST => count * 3,
        D3DPT_TRIANGLESTRIP | D3DPT_TRIANGLEFAN => count + 2,
        _ => 0,
    }
}

// ---- resource methods -------------------------------------------------------

pub unsafe extern "system" fn surf_get_desc(selfp: *mut ComObj, out: *mut D3DSURFACE_DESC) -> Hr {
    let s = &*(*selfp).data.cast::<Surface>();
    if out.is_null() {
        return D3DERR_INVALIDCALL;
    }
    *out = D3DSURFACE_DESC {
        Format: s.fmt,
        Type: D3DRTYPE_SURFACE,
        Usage: s.usage,
        Pool: s.pool,
        MultiSampleType: 0,
        MultiSampleQuality: 0,
        Width: s.w,
        Height: s.h,
    };
    S_OK
}

pub unsafe extern "system" fn surf_get_type(_self: *mut ComObj) -> u32 {
    match (&*_self).tag {
        Tag::Surface => D3DRTYPE_SURFACE,
        Tag::VertexBuffer => D3DRTYPE_VERTEXBUFFER,
        Tag::IndexBuffer => D3DRTYPE_INDEXBUFFER,
        Tag::Texture => D3DRTYPE_TEXTURE,
        _ => 0,
    }
}

/// IDirect3DSurface9::LockRect(self, lr, rect, flags) - stdcall, 4 args.
/// (IDirect3DTexture9::LockRect has the extra `level`; the surface one does not.)
pub unsafe extern "system" fn surf_lock_rect(
    selfp: *mut ComObj,
    lr: *mut D3DLOCKED_RECT,
    _rect: *const RECT,
    flags: u32,
) -> Hr {
    let s = &mut *((*selfp).data as *mut Surface);    if lr.is_null() {
        return D3DERR_INVALIDCALL;
    }
    let Some(sys) = &mut s.sys else {        return D3DERR_INVALIDCALL;
    };
    sys.dirty = flags & 0 == 0; // writes through the pointer must mark dirty
    (*lr).Pitch = sys.pitch as i32;
    (*lr).pBits = sys.data.as_mut_ptr() as *mut c_void;
    S_OK
}

/// IDirect3DSurface9::UnlockRect(self) - stdcall, 1 arg (no rect on Surface;
/// only IDirect3DTexture9::UnlockRect takes a level).
pub unsafe extern "system" fn surf_unlock_rect(_self: *mut ComObj) -> Hr {
    S_OK
}

pub unsafe extern "system" fn vb_lock(
    selfp: *mut ComObj,
    offset: u32,
    size: u32,
    out: *mut *mut c_void,
    flags: u32,
) -> Hr {
    let b = &mut *((*selfp).data as *mut Buffer);
    if out.is_null() {
        return D3DERR_INVALIDCALL;
    }
    let o = offset as usize;
    let n = if size == 0 { b.data.len() - o } else { size as usize };
    if o + n > b.data.len() {
        return D3DERR_INVALIDCALL;
    }
    b.locked = flags & 0 == 0;
    *out = b.data.as_mut_ptr().add(o) as *mut c_void;
    S_OK
}

pub unsafe extern "system" fn vb_unlock(selfp: *mut ComObj) -> Hr {
    let b = &mut *((*selfp).data as *mut Buffer);
    b.locked = false;
    S_OK
}

pub unsafe extern "system" fn vb_get_desc(selfp: *mut ComObj, out: *mut D3DVERTEXBUFFER_DESC) -> Hr {
    let b = &*(*selfp).data.cast::<Buffer>();
    if out.is_null() {
        return D3DERR_INVALIDCALL;
    }
    *out = D3DVERTEXBUFFER_DESC {
        Format: D3DFMT_VERTEXDATA,
        Type: D3DRTYPE_VERTEXBUFFER,
        Usage: b.usage,
        Pool: b.pool,
        Size: b.len,
        FVF: b.fvf,
    };
    S_OK
}

pub unsafe extern "system" fn tex_get_level_count(_self: *mut ComObj) -> u32 {
    let t = unsafe { &*((*_self).data as *mut Texture) };
    t.levels
}

pub unsafe extern "system" fn tex_get_level_desc(selfp: *mut ComObj, level: u32, out: *mut D3DSURFACE_DESC) -> Hr {
    let t = &*(*selfp).data.cast::<Texture>();
    if level >= t.levels || out.is_null() {
        return D3DERR_INVALIDCALL;
    }
    *out = D3DSURFACE_DESC {
        Format: t.fmt,
        Type: D3DRTYPE_TEXTURE,
        Usage: t.usage,
        Pool: t.pool,
        MultiSampleType: 0,
        MultiSampleQuality: 0,
        Width: t.w >> level,
        Height: t.h >> level,
    };
    S_OK
}

pub unsafe extern "system" fn tex_get_surface_level(selfp: *mut ComObj, level: u32, out: *mut *mut c_void) -> Hr {
    let t = &*(*selfp).data.cast::<Texture>();
    if level >= t.levels {
        return D3DERR_INVALIDCALL;
    }
    if level != 0 {
        warn_once(Tag::Texture, "GetSurfaceLevel(level>0)", D3DERR_INVALIDCALL);
        return D3DERR_INVALIDCALL;
    }
    let w = t.w;
    let h = t.h;
    let devp = t.device;
    // a light view onto level 0: shares the texture object itself
    unsafe { crate::vtbl::addref(selfp) };
    *out = selfp as *mut c_void;
    let _ = (w, h, devp);
    S_OK
}

pub unsafe extern "system" fn tex_lock_rect(
    selfp: *mut ComObj,
    level: u32,
    lr: *mut D3DLOCKED_RECT,
    _rect: *const RECT,
    _flags: u32,
) -> Hr {
    let t = &mut *((*selfp).data as *mut Texture);
    if lr.is_null() || level >= t.levels {
        return D3DERR_INVALIDCALL;
    }
    let l = &mut t.levels_sys[level as usize];
    l.dirty = true;
    (*lr).Pitch = l.pitch as i32;
    (*lr).pBits = l.data.as_mut_ptr() as *mut c_void;
    S_OK
}

pub unsafe extern "system" fn tex_unlock_rect(selfp: *mut ComObj, level: u32) -> Hr {
    let t = &mut *((*selfp).data as *mut Texture);
    if level >= t.levels {
        return D3DERR_INVALIDCALL;
    }
    // D3D9 semantics: unlocking only marks the level dirty; the upload happens
    // once per frame at flush_textures (EndScene / before draws). Uploading on
    // every unlock would issue one full-texture write per locked row.
    t.levels_sys[level as usize].dirty = true;
    S_OK
}

pub unsafe extern "system" fn tex_add_dirty_rect(_self: *mut ComObj, _rect: *const RECT) -> Hr {
    let t = &mut *((*_self).data as *mut Texture);
    if let Some(l) = t.levels_sys.get_mut(0) {
        l.dirty = true;
    }
    S_OK
}

// extra fmt constant used above
pub const D3DFMT_VERTEXDATA: u32 = 163;

/// IDirect3DDevice9::GetRenderTargetData: copy the scene (backbuffer) into a
/// system-memory surface. Used by the C shim's GetDC readback path.
pub unsafe extern "system" fn get_render_target_data(
    selfp: *mut ComObj,
    _src: *mut ComObj,
    dst: *mut ComObj,
) -> Hr {
    let d = dev(selfp);
    if dst.is_null() {
        return D3DERR_INVALIDCALL;
    }
    let ds = &mut *((*dst).data as *mut Surface);
    if (*dst).tag != Tag::Surface {
            return D3DERR_INVALIDCALL;
    }
    let Some(sys) = ds.sys.as_mut() else {
        return D3DERR_INVALIDCALL;
    };
    d.renderer.readback_scene(sys)
}
