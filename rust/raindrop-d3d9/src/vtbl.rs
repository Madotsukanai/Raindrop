// COM vtable construction + shared IUnknown implementation.
// Object layout: { vt: *const *const c_void, refs: i32, tag: Tag, data: *mut c_void }

use crate::com::*;
use crate::vtbl_gen::*;
use core::ffi::c_void;
use std::sync::OnceLock;

pub type Slot = *const c_void;

// -- resource-side shared state ----------------------------------------------

pub struct SwapChain {
    pub device: *mut ComObj,
}

pub struct Surface {
    pub device: *mut ComObj,
    pub w: u32,
    pub h: u32,
    pub fmt: u32,
    pub usage: u32,
    pub pool: u32,
    pub is_backbuffer: bool,
    pub sys: Option<SysLevel>,
}

pub struct Buffer {
    pub device: *mut ComObj,
    pub len: u32,
    pub fvf: u32,
    pub usage: u32,
    pub pool: u32,
    pub data: Vec<u8>,
    pub locked: bool,
}

pub struct SysLevel {
    pub pitch: usize, // bytes
    pub data: Vec<u8>,
    pub dirty: bool,
}

pub struct Texture {
    pub device: *mut ComObj,
    pub w: u32,
    pub h: u32,
    pub levels: u32,
    pub fmt: u32,
    pub usage: u32,
    pub pool: u32,
    pub levels_sys: Vec<SysLevel>,
    pub gpu: Option<wgpu::Texture>,
}

impl Texture {
    pub fn level0_mut(&mut self) -> &mut SysLevel {
        &mut self.levels_sys[0]
    }
}

// -- IUnknown ----------------------------------------------------------------

pub unsafe extern "system" fn qi(selfp: *mut ComObj, riid: *const u32, out: *mut *mut c_void) -> Hr {
    if riid.is_null() || out.is_null() {
        return D3DERR_INVALIDCALL;
    }
    let riid = unsafe { &*riid.cast::<(u32, u16, u16, [u8; 8])>() };
    let tag_iid: (u32, u16, u16, [u8; 8]) = match (&*selfp).tag {
        Tag::D3D9 => D3D9_IID,
        Tag::Device => DEVICE9_IID,
        Tag::SwapChain => SWAPCHAIN9_IID,
        Tag::Surface => SURFACE9_IID,
        Tag::VertexBuffer => VERTEXBUFFER9_IID,
        Tag::IndexBuffer => INDEXBUFFER9_IID,
        Tag::Texture => TEXTURE9_IID,
    };
    let iunknown: (u32, u16, u16, [u8; 8]) = (0, 0, 0, [0xC0, 0, 0, 0, 0, 0, 0, 0x46]);
    if *riid == tag_iid || *riid == iunknown {
        unsafe { addref(selfp) };
        *out = selfp as *mut c_void;
        S_OK
    } else {
        *out = core::ptr::null_mut();
        E_NOINTERFACE
    }
}

pub unsafe extern "system" fn addref(selfp: *mut ComObj) -> u32 {
    let o = &mut *selfp;
    o.refs += 1;
    o.refs as u32
}

pub unsafe extern "system" fn release(selfp: *mut ComObj) -> u32 {
    let o = &mut *selfp;
    o.refs -= 1;
    if o.refs == 0 {
        if !o.data.is_null() {
            match o.tag {
                Tag::D3D9 => drop(Box::from_raw(o.data.cast::<crate::d9::D3D9State>())),
                Tag::Device => drop(Box::from_raw(o.data.cast::<crate::device::Device>())),
                Tag::SwapChain => drop(Box::from_raw(o.data.cast::<SwapChain>())),
                Tag::Surface => drop(Box::from_raw(o.data.cast::<Surface>())),
                Tag::VertexBuffer | Tag::IndexBuffer => drop(Box::from_raw(o.data.cast::<Buffer>())),
                Tag::Texture => drop(Box::from_raw(o.data.cast::<Texture>())),
            }
        }
        drop(Box::from_raw(selfp));
    }
    o.refs.max(0) as u32
}

// -- generic stubs ------------------------------------------------------------

pub unsafe extern "system" fn unimpl(selfp: *mut ComObj) -> Hr {
    let o = &*selfp;
    warn_once(o.tag, "method", D3DERR_INVALIDCALL)
}

pub unsafe extern "system" fn unimpl_hwnd(selfp: *mut ComObj, _h: *mut c_void) -> Hr {
    let o = &*selfp;
    warn_once(o.tag, "method", D3DERR_INVALIDCALL)
}

pub unsafe extern "system" fn unimpl_ret_ptr(selfp: *mut ComObj) -> *mut c_void {
    let o = &*selfp;
    warn_once(o.tag, "method", D3DERR_INVALIDCALL);
    core::ptr::null_mut()
}

/// Allocate a COM object carrying `data` (already boxed raw pointer).
pub fn make_obj(tag: Tag, vt: *const *const c_void, data: *mut c_void) -> *mut ComObj {
    Box::into_raw(Box::new(ComObj { vt, refs: 1, tag, data }))
}

// -- vtable construction ------------------------------------------------------

pub struct Vtables(
    pub Box<[Slot]>,
    pub Box<[Slot]>,
    pub Box<[Slot]>,
    pub Box<[Slot]>,
    pub Box<[Slot]>,
    pub Box<[Slot]>,
    pub Box<[Slot]>,
);
// the slot tables are plain function pointers, only ever read; Send+Sync is sound here
unsafe impl Send for Vtables {}
unsafe impl Sync for Vtables {}

static VTS: OnceLock<Vtables> = OnceLock::new();

pub fn vtables() -> &'static Vtables {
    VTS.get_or_init(build_all)
}

fn build_all() -> Vtables {
    use crate::device as d;
    use crate::d9;
    Vtables(
        slots!(D3D9_SLOTS; [
            UNKNOWN_QUERYINTERFACE => qi, UNKNOWN_ADDREF => addref, UNKNOWN_RELEASE => release,
            D3D9_GETADAPTERCOUNT => d9::get_adapter_count,
            D3D9_GETADAPTERMODECOUNT => d9::get_adapter_mode_count,
            D3D9_ENUMADAPTERMODES => d9::enum_adapter_modes,
            D3D9_GETADAPTERDISPLAYMODE => d9::get_adapter_display_mode,
            D3D9_GETADAPTERIDENTIFIER => d9::get_adapter_identifier,
            D3D9_CHECKDEVICETYPE => d9::check_device_type,
            D3D9_CHECKDEVICEFORMAT => d9::check_device_format,
            D3D9_CHECKDEVICEMULTISAMPLETYPE => d9::check_ms,
            D3D9_CHECKDEPTHSTENCILMATCH => d9::check_ms,
            D3D9_GETDEVICECAPS => d9::get_device_caps,
            D3D9_GETADAPTERMONITOR => unimpl_ret_ptr,
            D3D9_CREATEDEVICE => d9::create_device,
        ]),
        slots!(DEVICE9_SLOTS; [
            UNKNOWN_QUERYINTERFACE => qi, UNKNOWN_ADDREF => addref, UNKNOWN_RELEASE => release,
            DEVICE9_TESTCOOPERATIVELEVEL => d::test_cooperative_level,
            DEVICE9_GETAVAILABLETEXTUREMEM => d::get_available_texture_mem,
            DEVICE9_EVICTMANAGEDRESOURCES => d::evict,
            DEVICE9_GETDIRECT3D => d::get_direct3d,
            DEVICE9_GETDEVICECAPS => d::get_device_caps,
            DEVICE9_GETDISPLAYMODE => d::get_display_mode,
            DEVICE9_GETCREATIONPARAMETERS => d::get_creation_parameters,
            DEVICE9_SETCURSORPROPERTIES => d::ok3,
            DEVICE9_SETCURSORPOSITION => d::set_cursor_position,
            DEVICE9_SHOWCURSOR => d::show_cursor,
            DEVICE9_RESET => d::reset,
            DEVICE9_PRESENT => d::present,
            DEVICE9_GETBACKBUFFER => d::get_back_buffer,
            DEVICE9_GETRASTERSTATUS => d::get_raster_status,
            DEVICE9_SETDIALOGBOXMODE => d::ok0,
            DEVICE9_SETGAMMARAMP => d::ok1,
            DEVICE9_GETGAMMARAMP => d::get_gamma_ramp,
            DEVICE9_CREATETEXTURE => d::create_texture,
            DEVICE9_CREATEVOLUMETEXTURE => d::notavail,
            DEVICE9_CREATECUBETEXTURE => d::notavail,
            DEVICE9_CREATEVERTEXBUFFER => d::create_vertex_buffer,
            DEVICE9_CREATEINDEXBUFFER => d::create_index_buffer,
            DEVICE9_CREATERENDERTARGET => d::create_render_target,
            DEVICE9_CREATEDEPTHSTENCILSURFACE => d::create_depth_stencil,
            DEVICE9_UPDATESURFACE => d::update_surface,
            DEVICE9_UPDATETEXTURE => d::update_texture,
            DEVICE9_GETRENDERTARGETDATA => d::notavail,
            DEVICE9_GETFRONTBUFFERDATA => d::notavail,
            DEVICE9_STRETCHRECT => d::stretch_rect,
            DEVICE9_COLORFILL => d::color_fill,
            DEVICE9_CREATEOFFSCREENPLAINSURFACE => d::create_offscreen_surface,
            DEVICE9_SETRENDERTARGET => d::set_render_target,
            DEVICE9_GETRENDERTARGET => d::get_render_target,
            DEVICE9_SETDEPTHSTENCILSURFACE => d::set_depth_stencil,
            DEVICE9_GETDEPTHSTENCILSURFACE => d::get_depth_stencil,
            DEVICE9_BEGINSCENE => d::begin_scene,
            DEVICE9_ENDSCENE => d::end_scene,
            DEVICE9_CLEAR => d::clear,
            DEVICE9_SETTRANSFORM => d::set_transform,
            DEVICE9_GETTRANSFORM => d::get_transform,
            DEVICE9_SETVIEWPORT => d::set_viewport,
            DEVICE9_GETVIEWPORT => d::get_viewport,
            DEVICE9_SETMATERIAL => d::ok1,
            DEVICE9_SETLIGHT => d::ok2,
            DEVICE9_LIGHTENABLE => d::ok2,
            DEVICE9_GETLIGHTENABLE => d::unimpl2,
            DEVICE9_SETCLIPPLANE => d::ok2,
            DEVICE9_SETRENDERSTATE => d::set_render_state,
            DEVICE9_GETRENDERSTATE => d::get_render_state,
            DEVICE9_SETCLIPSTATUS => d::ok1,
            DEVICE9_GETCLIPSTATUS => d::notavail,
            DEVICE9_GETTEXTURE => d::get_texture,
            DEVICE9_SETTEXTURE => d::set_texture,
            DEVICE9_GETTEXTURESTAGESTATE => d::get_texture_stage_state,
            DEVICE9_SETTEXTURESTAGESTATE => d::set_texture_stage_state,
            DEVICE9_GETSAMPLERSTATE => d::get_sampler_state,
            DEVICE9_SETSAMPLERSTATE => d::set_sampler_state,
            DEVICE9_VALIDATEDEVICE => d::ok0,
            DEVICE9_SETPALETTEENTRIES => d::ok2,
            DEVICE9_SETCURRENTTEXTUREPALETTE => d::ok1,
            DEVICE9_SETSCISSORRECT => d::ok1,
            DEVICE9_SETSOFTWAREVERTEXPROCESSING => d::set_svp,
            DEVICE9_GETSOFTWAREVERTEXPROCESSING => d::get_svp,
            DEVICE9_SETNPATCHMODE => d::ok1,
            DEVICE9_DRAWPRIMITIVE => d::draw_primitive,
            DEVICE9_DRAWINDEXEDPRIMITIVE => d::draw_indexed_primitive,
            DEVICE9_DRAWPRIMITIVEUP => d::draw_primitive_up,
            DEVICE9_DRAWINDEXEDPRIMITIVEUP => d::draw_indexed_primitive_up,
            DEVICE9_CREATEVERTEXDECLARATION => d::notavail,
            DEVICE9_SETFVF => d::set_fvf,
            DEVICE9_GETFVF => d::get_fvf,
            DEVICE9_CREATEVERTEXSHADER => d::notavail,
            DEVICE9_SETVERTEXSHADER => d::set_vertex_shader,
            DEVICE9_SETVERTEXSHADERCONSTANTF => d::set_vsc_f,
            DEVICE9_SETPIXELSHADER => d::set_pixel_shader,
            DEVICE9_SETPIXELSHADERCONSTANTF => d::set_psc_f,
            DEVICE9_SETSTREAMSOURCE => d::set_stream_source,
            DEVICE9_SETINDICES => d::set_indices,
            DEVICE9_CREATEQUERY => d::notavail,
        ]),
        slots!(SWAPCHAIN9_SLOTS; [
            UNKNOWN_QUERYINTERFACE => qi, UNKNOWN_ADDREF => addref, UNKNOWN_RELEASE => release,
            SWAPCHAIN9_PRESENT => d::swapchain_present,
            SWAPCHAIN9_GETBACKBUFFER => d::get_back_buffer,
        ]),
        slots!(SURFACE9_SLOTS; [
            UNKNOWN_QUERYINTERFACE => qi, UNKNOWN_ADDREF => addref, UNKNOWN_RELEASE => release,
            SURFACE9_GETDEVICE => d::get_direct3d_surf,
            SURFACE9_GETDESC => d::surf_get_desc,
            SURFACE9_LOCKRECT => d::surf_lock_rect,
            SURFACE9_UNLOCKRECT => d::surf_unlock_rect,
            SURFACE9_GETDC => unimpl_ret_ptr,
            SURFACE9_RELEASEDC => unimpl_hwnd,
            SURFACE9_GETTYPE => d::surf_get_type,
        ]),
        slots!(VERTEXBUFFER9_SLOTS; [
            UNKNOWN_QUERYINTERFACE => qi, UNKNOWN_ADDREF => addref, UNKNOWN_RELEASE => release,
            VERTEXBUFFER9_GETDEVICE => d::get_direct3d_surf,
            VERTEXBUFFER9_GETTYPE => d::surf_get_type,
            VERTEXBUFFER9_LOCK => d::vb_lock,
            VERTEXBUFFER9_UNLOCK => d::vb_unlock,
            VERTEXBUFFER9_GETDESC => d::vb_get_desc,
        ]),
        slots!(INDEXBUFFER9_SLOTS; [
            UNKNOWN_QUERYINTERFACE => qi, UNKNOWN_ADDREF => addref, UNKNOWN_RELEASE => release,
            INDEXBUFFER9_GETDEVICE => d::get_direct3d_surf,
            INDEXBUFFER9_GETTYPE => d::surf_get_type,
            INDEXBUFFER9_LOCK => d::vb_lock,
            INDEXBUFFER9_UNLOCK => d::vb_unlock,
            INDEXBUFFER9_GETDESC => d::vb_get_desc,
        ]),
        slots!(TEXTURE9_SLOTS; [
            UNKNOWN_QUERYINTERFACE => qi, UNKNOWN_ADDREF => addref, UNKNOWN_RELEASE => release,
            TEXTURE9_GETDEVICE => d::get_direct3d_surf,
            TEXTURE9_GETTYPE => d::surf_get_type,
            TEXTURE9_GETLEVELCOUNT => d::tex_get_level_count,
            TEXTURE9_GETLEVELDESC => d::tex_get_level_desc,
            TEXTURE9_GETSURFACELEVEL => d::tex_get_surface_level,
            TEXTURE9_LOCKRECT => d::tex_lock_rect,
            TEXTURE9_UNLOCKRECT => d::tex_unlock_rect,
            TEXTURE9_ADDDIRTYRECT => d::tex_add_dirty_rect,
        ]),
    )
}

/// Build a slot table: every slot gets `unimpl`, listed overrides replace them.
macro_rules! slots {
    ($n:expr; [ $($idx:expr => $f:expr),* $(,)? ]) => {{
        let mut v: Vec<Slot> = vec![unimpl as *const () as usize as Slot; $n];
        $( v[$idx] = $f as *const () as usize as Slot; )*
        v.into_boxed_slice()
    }};
}
use slots;
