// D3D9 constants (subset used by the emulator). Values verified against mingw d3d9types.h.

use crate::com::{Hr, D3DERR_INVALIDCALL, D3DERR_NOTAVAILABLE, S_OK, D3D_OK};
// D3DFORMAT
pub const D3DFMT_UNKNOWN: u32 = 0;
pub const D3DFMT_A8R8G8B8: u32 = 21;
pub const D3DFMT_X8R8G8B8: u32 = 22;
pub const D3DFMT_R5G6B5: u32 = 23;
pub const D3DFMT_X1R5G5B5: u32 = 24;
pub const D3DFMT_A1R5G5B5: u32 = 25;
pub const D3DFMT_A4R4G4B4: u32 = 26;
pub const D3DFMT_A8: u32 = 28;
pub const D3DFMT_D24S8: u32 = 75;
pub const D3DFMT_D16: u32 = 80;

// D3DPOOL
pub const D3DPOOL_DEFAULT: u32 = 0;
pub const D3DPOOL_MANAGED: u32 = 1;
pub const D3DPOOL_SYSTEMMEM: u32 = 2;
pub const D3DPOOL_SCRATCH: u32 = 3;

// D3DUSAGE
pub const D3DUSAGE_RENDERTARGET: u32 = 0x1;
pub const D3DUSAGE_DEPTHSTENCIL: u32 = 0x2;
pub const D3DUSAGE_DYNAMIC: u32 = 0x200;
pub const D3DUSAGE_AUTOGENMIPMAP: u32 = 0x400;

// D3DFVF
pub const D3DFVF_XYZ: u32 = 0x0002;
pub const D3DFVF_XYZRHW: u32 = 0x0004;
pub const D3DFVF_DIFFUSE: u32 = 0x0040;
pub const D3DFVF_SPECULAR: u32 = 0x0080;
pub const D3DFVF_TEX1: u32 = 0x0100;
pub const D3DFVF_POSITION_MASK: u32 = 0x400E;

// D3DPRIMITIVETYPE
pub const D3DPT_POINTLIST: u32 = 1;
pub const D3DPT_LINELIST: u32 = 2;
pub const D3DPT_LINESTRIP: u32 = 3;
pub const D3DPT_TRIANGLELIST: u32 = 4;
pub const D3DPT_TRIANGLESTRIP: u32 = 5;
pub const D3DPT_TRIANGLEFAN: u32 = 6;

// D3DCLEAR
pub const D3DCLEAR_TARGET: u32 = 0x1;
pub const D3DCLEAR_ZBUFFER: u32 = 0x2;
pub const D3DCLEAR_STENCIL: u32 = 0x4;

// D3DRENDERSTATETYPE (subset)
pub const D3DRS_ZENABLE: u32 = 7;
pub const D3DRS_FILLMODE: u32 = 8;
pub const D3DRS_SHADEMODE: u32 = 9;
pub const D3DRS_ALPHATESTENABLE: u32 = 15;
pub const D3DRS_SRCBLEND: u32 = 19;
pub const D3DRS_DESTBLEND: u32 = 20;
pub const D3DRS_CULLMODE: u32 = 22;
pub const D3DRS_ALPHAREF: u32 = 24;
pub const D3DRS_ALPHAFUNC: u32 = 25;
pub const D3DRS_ALPHABLENDENABLE: u32 = 27;
pub const D3DRS_BLENDOP: u32 = 171;

// D3DBLEND
pub const D3DBLEND_ZERO: u32 = 1;
pub const D3DBLEND_ONE: u32 = 2;
pub const D3DBLEND_SRCCOLOR: u32 = 3;
pub const D3DBLEND_INVSRCCOLOR: u32 = 4;
pub const D3DBLEND_SRCALPHA: u32 = 5;
pub const D3DBLEND_INVSRCALPHA: u32 = 6;
pub const D3DBLEND_DESTALPHA: u32 = 7;
pub const D3DBLEND_INVDESTALPHA: u32 = 8;
pub const D3DBLEND_DESTCOLOR: u32 = 9;
pub const D3DBLEND_INVDESTCOLOR: u32 = 10;
pub const D3DBLEND_SRCALPHASAT: u32 = 11;

// D3DCMPFUNC
pub const D3DCMP_NEVER: u32 = 1;
pub const D3DCMP_LESS: u32 = 2;
pub const D3DCMP_EQUAL: u32 = 3;
pub const D3DCMP_LESSEQUAL: u32 = 4;
pub const D3DCMP_GREATER: u32 = 5;
pub const D3DCMP_NOTEQUAL: u32 = 6;
pub const D3DCMP_GREATEREQUAL: u32 = 7;
pub const D3DCMP_ALWAYS: u32 = 8;

// D3DTEXTURESTAGESTATETYPE (subset)
pub const D3DTSS_COLOROP: u32 = 1;
pub const D3DTSS_ALPHAOP: u32 = 4;
pub const D3DTOP_DISABLE: u32 = 1;
pub const D3DTOP_SELECTARG1: u32 = 2;
pub const D3DTOP_MODULATE: u32 = 4;

// D3DSAMPLERSTATETYPE
pub const D3DSAMP_ADDRESSU: u32 = 1;
pub const D3DSAMP_ADDRESSV: u32 = 2;
pub const D3DSAMP_MAGFILTER: u32 = 5;
pub const D3DSAMP_MINFILTER: u32 = 6;
pub const D3DSAMP_MIPFILTER: u32 = 7;

// D3DTRANSFORMSTATETYPE
pub const D3DTS_WORLD: u32 = 1;
pub const D3DTS_VIEW: u32 = 2;
pub const D3DTS_PROJECTION: u32 = 3;

// D3DSWAPEFFECT
pub const D3DSWAPEFFECT_DISCARD: u32 = 1;
pub const D3DSWAPEFFECT_FLIP: u32 = 2;
pub const D3DSWAPEFFECT_COPY: u32 = 3;

// D3DPRESENT intervals
pub const D3DPRESENT_INTERVAL_DEFAULT: u32 = 0;
pub const D3DPRESENT_INTERVAL_ONE: u32 = 1;
pub const D3DPRESENT_INTERVAL_IMMEDIATE: u32 = 0x8000_0000;

// D3DDEVTYPE
pub const D3DDEVTYPE_HAL: u32 = 1;

// D3DRESOURCETYPE
pub const D3DRTYPE_SURFACE: u32 = 1;
pub const D3DRTYPE_TEXTURE: u32 = 3;
pub const D3DRTYPE_VERTEXBUFFER: u32 = 6;
pub const D3DRTYPE_INDEXBUFFER: u32 = 7;

// caps bits we advertise
pub const D3DDEVCAPS_HWRASTERIZATION: u32 = 0x800;
pub const D3DDEVCAPS_CANRENDERAFTERFLIP: u32 = 0x1000;
pub const D3DDEVCAPS_TEXTURENONLOCALVIDMEM: u32 = 0x200;
pub const D3DDEVCAPS_DRAWPRIMTLVERTEX: u32 = 0x400;
pub const D3DPRASTERCAPS_DITHER: u32 = 0x1;
pub const D3DPMISCCAPS_CULLNONE: u32 = 0x10;
pub const D3DPMISCCAPS_CULLCCW: u32 = 0x20;
pub const D3DPMISCCAPS_CULLCW: u32 = 0x40;
pub const D3DPSHADECAPS_ALPHAGLBLEND: u32 = 0x100;
pub const D3DPTCAPS_ALPHA: u32 = 0x4;
pub const D3DTEXCAPS_ALPHA: u32 = 0x4;
pub const D3DVTXPCAPS_TEXTUREVERTEX: u32 = 0x10000;
pub const D3DVTXPCAPS_NO_VSDT: u32 = 0x200000;
pub const D3DCAPS2_CANRENDERWINDOWED: u32 = 0x80000;
pub const D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD: u32 = 0x20;
pub const D3DCAPS2_CANMANAGERESOURCE: u32 = 0x10000000;
pub const D3DCAPS2_DYNAMICTEXTURES: u32 = 0x20000000;
pub const D3DPCAPS_SOFTRASTERIZATION: u32 = 0x200;

/// Bytes per pixel of a supported D3DFMT (0 = unsupported).
pub fn bytes_per_pixel(fmt: u32) -> u32 {
    match fmt {
        D3DFMT_A8R8G8B8 | D3DFMT_X8R8G8B8 => 4,
        D3DFMT_R5G6B5 | D3DFMT_X1R5G5B5 | D3DFMT_A1R5G5B5 | D3DFMT_A4R4G4B4 => 2,
        D3DFMT_A8 => 1,
        _ => 0,
    }
}

/// GPU mirror format for a supported D3DFMT. Sub-32bpp formats are uploaded
/// through the fearless_simd conversion kernels into Bgra8Unorm.
#[cfg(feature = "wgpu-backend")]
pub fn wgpu_of_fmt(fmt: u32) -> Option<wgpu::TextureFormat> {
    if bytes_per_pixel(fmt) == 0 {
        return None;
    }
    Some(wgpu::TextureFormat::Bgra8Unorm)
}
