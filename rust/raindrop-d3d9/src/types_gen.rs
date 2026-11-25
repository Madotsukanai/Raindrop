use core::ffi::c_void;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct D3DCAPS9 {
    pub DeviceType: u32,
    pub AdapterOrdinal: u32,
    pub Caps: u32,
    pub Caps2: u32,
    pub Caps3: u32,
    pub PresentationIntervals: u32,
    pub CursorCaps: u32,
    pub DevCaps: u32,
    pub PrimitiveMiscCaps: u32,
    pub RasterCaps: u32,
    pub ZCmpCaps: u32,
    pub SrcBlendCaps: u32,
    pub DestBlendCaps: u32,
    pub AlphaCmpCaps: u32,
    pub ShadeCaps: u32,
    pub TextureCaps: u32,
    pub TextureFilterCaps: u32,
    pub CubeTextureFilterCaps: u32,
    pub VolumeTextureFilterCaps: u32,
    pub TextureAddressCaps: u32,
    pub VolumeTextureAddressCaps: u32,
    pub LineCaps: u32,
    pub MaxTextureHeight: u32,
    pub MaxTextureWidth: u32,
    pub MaxVolumeExtent: u32,
    pub MaxTextureRepeat: u32,
    pub MaxTextureAspectRatio: u32,
    pub MaxAnisotropy: u32,
    pub MaxVertexW: f32,
    pub GuardBandLeft: f32,
    pub GuardBandTop: f32,
    pub GuardBandRight: f32,
    pub GuardBandBottom: f32,
    pub ExtentsAdjust: f32,
    pub StencilCaps: u32,
    pub FVFCaps: u32,
    pub TextureOpCaps: u32,
    pub MaxTextureBlendStages: u32,
    pub MaxSimultaneousTextures: u32,
    pub VertexProcessingCaps: u32,
    pub MaxActiveLights: u32,
    pub MaxUserClipPlanes: u32,
    pub MaxVertexBlendMatrices: u32,
    pub MaxVertexBlendMatrixIndex: u32,
    pub MaxPointSize: f32,
    pub MaxPrimitiveCount: u32,
    pub MaxVertexIndex: u32,
    pub MaxStreams: u32,
    pub MaxStreamStride: u32,
    pub VertexShaderVersion: u32,
    pub MaxVertexShaderConst: u32,
    pub PixelShaderVersion: u32,
    pub PixelShader1xMaxValue: f32,
    pub DevCaps2: u32,
    pub MaxNpatchTessellationLevel: f32,
    pub Reserved5: u32,
    pub MasterAdapterOrdinal: u32,
    pub AdapterOrdinalInGroup: u32,
    pub NumberOfAdaptersInGroup: u32,
    pub DeclTypes: u32,
    pub NumSimultaneousRTs: u32,
    pub StretchRectFilterCaps: u32,
    pub VS20Caps: D3DVSHADERCAPS2_0,
    pub PS20Caps: D3DPSHADERCAPS2_0,
    pub VertexTextureFilterCaps: u32,
    pub MaxVShaderInstructionsExecuted: u32,
    pub MaxPShaderInstructionsExecuted: u32,
    pub MaxVertexShader30InstructionSlots: u32,
    pub MaxPixelShader30InstructionSlots: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct D3DDISPLAYMODE {
    pub Width: u32,
    pub Height: u32,
    pub RefreshRate: u32,
    pub Format: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct D3DVIEWPORT9 {
    pub X: u32,
    pub Y: u32,
    pub Width: u32,
    pub Height: u32,
    pub MinZ: f32,
    pub MaxZ: f32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct D3DVERTEXBUFFER_DESC {
    pub Format: u32,
    pub Type: u32,
    pub Usage: u32,
    pub Pool: u32,
    pub Size: u32,
    pub FVF: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct D3DINDEXBUFFER_DESC {
    pub Format: u32,
    pub Type: u32,
    pub Usage: u32,
    pub Pool: u32,
    pub Size: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct D3DSURFACE_DESC {
    pub Format: u32,
    pub Type: u32,
    pub Usage: u32,
    pub Pool: u32,
    pub MultiSampleType: u32,
    pub MultiSampleQuality: u32,
    pub Width: u32,
    pub Height: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct D3DDEVICE_CREATION_PARAMETERS {
    pub AdapterOrdinal: u32,
    pub DeviceType: u32,
    pub hFocusWindow: *mut c_void,
    pub BehaviorFlags: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct D3DRASTER_STATUS {
    pub InVBlank: i32,
    pub ScanLine: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct D3DGAMMARAMP {
    pub red: [u16; 256],
    pub green: [u16; 256],
    pub blue: [u16; 256],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct D3DADAPTER_IDENTIFIER9 {
    pub Driver: [i8; 512],
    pub Description: [i8; 512],
    pub DeviceName: [i8; 32],
    pub DriverVersionLowPart: u32,
    pub DriverVersionHighPart: u32,
    pub VendorId: u32,
    pub DeviceId: u32,
    pub SubSysId: u32,
    pub Revision: u32,
    pub DeviceIdentifier: [u8; 16],
    pub WHQLLevel: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct D3DLOCKED_RECT {
    pub Pitch: i32,
    pub pBits: *mut c_void,
}


#[repr(C)]
#[derive(Clone, Copy)]
pub struct D3DPRESENT_PARAMETERS {
    pub BackBufferWidth: u32,
    pub BackBufferHeight: u32,
    pub BackBufferFormat: u32,
    pub BackBufferCount: u32,
    pub MultiSampleType: u32,
    pub MultiSampleQuality: u32,
    pub SwapEffect: u32,
    pub hDeviceWindow: *mut c_void,
    pub Windowed: i32,
    pub EnableAutoDepthStencil: i32,
    pub AutoDepthStencilFormat: u32,
    pub Flags: u32,
    pub FullScreen_RefreshRateInHz: u32,
    pub PresentationInterval: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct D3DVSHADERCAPS2_0 {
    pub _unused: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
pub struct D3DPSHADERCAPS2_0 {
    pub _unused: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct RECT {
    pub left: i32,
    pub top: i32,
    pub right: i32,
    pub bottom: i32,
}
