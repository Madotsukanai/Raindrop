// IDirect3D9 emulation: adapter enumeration + CreateDevice into the wgpu backend.

use crate::com::*;
use crate::d3d9::*;
use crate::types_gen::*;
use crate::vtbl::*;
use core::ffi::c_void;

pub struct D3D9State {
    pub ctx: Option<std::sync::Arc<raindrop_gpu::gpu::RgContext>>,
    /// presentation surface created together with the ctx (adapter-matched);
    /// handed to the first device's Renderer
    pub surface: Option<wgpu::Surface<'static>>,
    /// mode table per adapter (only adapter 0 exists)
    pub modes: [(u32, u32, u32); 4], // w, h, refresh
}

impl D3D9State {
    fn new() -> Self {
        D3D9State {
            ctx: None,
            surface: None,
            modes: [(640, 480, 60), (800, 600, 60), (1024, 768, 60), (1280, 1024, 60)],
        }
    }
}

pub fn create_d3d9(sdk_version: u32) -> *mut ComObj {
    if sdk_version < 31 {
        // refuse older-than-DX9 SDK requests like the runtime does
        warn_once(Tag::D3D9, "unsupported SDK version", D3DERR_NOTAVAILABLE);
        return core::ptr::null_mut();
    }
    let vts = vtables();
    let data = Box::into_raw(Box::new(D3D9State::new())) as *mut c_void;
    make_obj(Tag::D3D9, vts.0.as_ptr(), data)
}

pub unsafe extern "system" fn get_adapter_count(_self: *mut ComObj) -> u32 {
    1
}

pub unsafe extern "system" fn get_adapter_mode_count(_self: *mut ComObj, adapter: u32, _fmt: u32) -> u32 {
    if adapter != 0 {
        return 0;
    }
    let st = &*(*_self).data.cast::<D3D9State>();
    st.modes.len() as u32
}

pub unsafe extern "system" fn enum_adapter_modes(
    _self: *mut ComObj,
    adapter: u32,
    _fmt: u32,
    index: u32,
    mode: *mut D3DDISPLAYMODE,
) -> Hr {
    if adapter != 0 || mode.is_null() {
        return D3DERR_INVALIDCALL;
    }
    let st = &*(*_self).data.cast::<D3D9State>();
    let m = match st.modes.get(index as usize) {
        Some(m) => *m,
        None => return D3DERR_INVALIDCALL,
    };
    *mode = D3DDISPLAYMODE { Width: m.0, Height: m.1, RefreshRate: m.2, Format: D3DFMT_X8R8G8B8 };
    S_OK
}

pub unsafe extern "system" fn get_adapter_display_mode(
    _self: *mut ComObj,
    adapter: u32,
    mode: *mut D3DDISPLAYMODE,
) -> Hr {
    if adapter != 0 || mode.is_null() {
        return D3DERR_INVALIDCALL;
    }
    let st = &*(*_self).data.cast::<D3D9State>();
    let m = st.modes[0];
    *mode = D3DDISPLAYMODE { Width: m.0, Height: m.1, RefreshRate: m.2, Format: D3DFMT_X8R8G8B8 };
    S_OK
}

pub unsafe extern "system" fn get_adapter_identifier(
    _self: *mut ComObj,
    adapter: u32,
    _flags: u32,
    out: *mut D3DADAPTER_IDENTIFIER9,
) -> Hr {
    if adapter != 0 || out.is_null() {
        return D3DERR_INVALIDCALL;
    }
    let mut id = D3DADAPTER_IDENTIFIER9 {
        Driver: [0; 512],
        Description: [0; 512],
        DeviceName: [0; 32],
        DriverVersionLowPart: 0,
        DriverVersionHighPart: 0,
        VendorId: 0,
        DeviceId: 0,
        SubSysId: 0,
        Revision: 0,
        DeviceIdentifier: [0; 16],
        WHQLLevel: 0,
    };
    let name = b"Raindrop wgpu adapter\0";
    id.Description[..name.len()].copy_from_slice(unsafe { core::mem::transmute::<&[u8], &[i8]>(name) });
    let drv = b"raindrop-d3d9\0";
    id.Driver[..drv.len()].copy_from_slice(unsafe { core::mem::transmute::<&[u8], &[i8]>(drv) });
    *out = id;
    S_OK
}

pub unsafe extern "system" fn check_device_type(
    _self: *mut ComObj,
    adapter: u32,
    _dt: u32,
    adapter_fmt: u32,
    back_fmt: u32,
    _windowed: i32,
) -> Hr {
    if adapter != 0 {
        return D3DERR_INVALIDCALL;
    }
    if bytes_per_pixel(adapter_fmt) == 0 || bytes_per_pixel(back_fmt) == 0 {
        return D3DERR_NOTAVAILABLE;
    }
    S_OK
}

pub unsafe extern "system" fn check_device_format(
    _self: *mut ComObj,
    adapter: u32,
    _dt: u32,
    _adapter_fmt: u32,
    _usage: u32,
    _rtype: u32,
    check_fmt: u32,
) -> Hr {
    if adapter != 0 {
        return D3DERR_INVALIDCALL;
    }
    if bytes_per_pixel(check_fmt) == 0 {
        return D3DERR_NOTAVAILABLE;
    }
    S_OK
}

pub unsafe extern "system" fn check_ms(
    _self: *mut ComObj,
    adapter: u32,
    _dt: u32,
    _fmt: u32,
    ms: u32,
    _windowed: i32,
) -> Hr {
    if adapter != 0 {
        return D3DERR_INVALIDCALL;
    }
    if ms != 0 {
        return D3DERR_NOTAVAILABLE;
    }
    S_OK
}

pub unsafe extern "system" fn get_device_caps(
    _self: *mut ComObj,
    adapter: u32,
    _dt: u32,
    caps: *mut D3DCAPS9,
) -> Hr {
    if adapter != 0 || caps.is_null() {
        return D3DERR_INVALIDCALL;
    }
    let mut c: D3DCAPS9 = unsafe { core::mem::zeroed() };
    c.DeviceType = D3DDEVTYPE_HAL;
    c.AdapterOrdinal = 0;
    c.Caps = 0;
    c.Caps2 = D3DCAPS2_CANRENDERWINDOWED | D3DCAPS2_CANMANAGERESOURCE | D3DCAPS2_DYNAMICTEXTURES;
    c.Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD;
    c.PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE;
    c.DevCaps = D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_DRAWPRIMTLVERTEX;
    c.PrimitiveMiscCaps = D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCCW | D3DPMISCCAPS_CULLCW;
    c.RasterCaps = D3DPRASTERCAPS_DITHER;
    c.SrcBlendCaps = 0x3FFE; // all blend factors
    c.DestBlendCaps = 0x3FFE;
    c.AlphaCmpCaps = 0x000000FE; // all cmp funcs (NEVER..ALWAYS)
    c.ShadeCaps = D3DPSHADECAPS_ALPHAGLBLEND;
    c.TextureCaps = D3DTEXCAPS_ALPHA | D3DTEXCAPS_ALPHA;
    c.TextureFilterCaps = 0x00000404; // MIN/MAG LINEAR+POINT
    c.MaxTextureBlendStages = 8;
    c.MaxSimultaneousTextures = 8;
    c.MaxTextureWidth = 8192;
    c.MaxTextureHeight = 8192;
    c.MaxTextureAspectRatio = 8192;
    c.MaxVertexW = 1e10f32;
    c.TextureOpCaps = 0x3FFFFFFE;
    c.VertexProcessingCaps = D3DVTXPCAPS_TEXTUREVERTEX | D3DVTXPCAPS_NO_VSDT;
    c.MaxPrimitiveCount = 0xFFFFF;
    c.MaxVertexIndex = 0xFFFFF;
    c.MaxStreams = 1;
    *caps = c;
    S_OK
}

pub unsafe extern "system" fn create_device(
    _self: *mut ComObj,
    adapter: u32,
    dt: u32,
    focus: *mut c_void,
    behavior: u32,
    pp: *mut D3DPRESENT_PARAMETERS,
    out: *mut *mut c_void,
) -> Hr {
    if adapter != 0 || dt != D3DDEVTYPE_HAL || pp.is_null() || out.is_null() {
        return D3DERR_INVALIDCALL;
    }
    let params = unsafe { &*pp };
    let state = &mut *(*_self).data.cast::<D3D9State>();
    match crate::device::Device::create(state, focus, params, behavior) {
        Ok(dev) => {
            // fix up the caller's present parameters like the runtime does
            unsafe {
                (*pp).BackBufferFormat = D3DFMT_X8R8G8B8;
                (*pp).MultiSampleType = 0;
                (*pp).BackBufferCount = 1;
            }
            let vts = vtables();
            let data = Box::into_raw(Box::new(dev)) as *mut c_void;
            let obj = make_obj(Tag::Device, vts.1.as_ptr(), data);
            let dev_ref = &mut *(data as *mut crate::device::Device);
            dev_ref.d3d9_obj = _self;
            dev_ref.create_backbuffer();
            *out = obj as *mut c_void;
            S_OK
        }
        Err(code) => code,
    }
}
