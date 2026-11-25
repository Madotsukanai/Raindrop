// Raindrop GPU - wgpu backend skeleton (feature: "wgpu-backend").
//
// Target topology (see rust/README.md): the C shim keeps the DirectDraw COM layer and
// D7Surf system-memory bitmaps; the wgpu side owns GPU textures + the future swapchain.
//
// Design notes:
//   * 32-bit process -> DX12 is impossible; backends are Vulkan (+ GL fallback).
//   * Textures mirror D7Surf bitmaps; CPU kernels (kernels.rs) convert into ARGB8888
//     staging rows, then `raindrop_texture_upload` issues queue.write_texture.
//   * Surface/swapchain creation from an HWND is the next step (raw-window-handle);
//     fixed-function quad emulation via WGSL after that.

#![allow(dead_code)]

use wgpu::{
    Adapter, Backends, Device, Extent3d, Instance, InstanceDescriptor, InstanceFlags, Origin3d,
    PowerPreference, Queue, RequestAdapterOptions, Texture, TextureAspect, TextureDescriptor,
    TextureDimension, TextureFormat, TextureUsages, TextureView, TextureViewDescriptor,
    TexelCopyBufferLayout, TexelCopyTextureInfo,
};

pub struct RgContext {
    #[allow(dead_code)]
    instance: Instance,
    adapter: Adapter,
    device: Device,
    queue: Queue,
}

impl RgContext {
    pub fn device(&self) -> &Device {
        &self.device
    }
    pub fn queue(&self) -> &Queue {
        &self.queue
    }
    pub fn instance(&self) -> &Instance {
        &self.instance
    }
    pub fn adapter(&self) -> &Adapter {
        &self.adapter
    }
}

pub struct RgTexture {
    pub(crate) texture: Texture,
    pub(crate) view: TextureView,
    pub(crate) width: u32,
    pub(crate) height: u32,
}

impl RgTexture {
    pub fn texture(&self) -> &Texture {
        &self.texture
    }
    pub fn view(&self) -> &TextureView {
        &self.view
    }
    pub fn size(&self) -> (u32, u32) {
        (self.width, self.height)
    }
}

/// Result of `init`: the GPU context plus an optional presentation surface
/// (owned here so the adapter can be selected with `compatible_surface`).
pub struct GpuInit {
    pub ctx: RgContext,
    pub surface: Option<wgpu::Surface<'static>>,
}

/// Raw Win32 surface for an HWND (None when hwnd == 0 or creation fails).
unsafe extern "system" {
    fn GetModuleHandleW(name: *const u16) -> isize;
}

fn raw_surface(instance: &Instance, hwnd: isize) -> Option<wgpu::Surface<'static>> {
    if hwnd == 0 {
        return None;
    }
    use core::num::NonZeroIsize;
    use raw_window_handle::{RawDisplayHandle, RawWindowHandle, WindowsDisplayHandle, Win32WindowHandle};
    use wgpu::SurfaceTargetUnsafe;
    let mut wh = Win32WindowHandle::new(NonZeroIsize::new(hwnd)?);
    // wgpu's Vulkan backend needs the Win32 hinstance on the raw handle
    let hinstance = unsafe { GetModuleHandleW(core::ptr::null()) } as isize;
    wh.hinstance = NonZeroIsize::new(hinstance);
    let rdh = RawDisplayHandle::Windows(WindowsDisplayHandle::new());
    let rwh = RawWindowHandle::Win32(wh);
    let target = SurfaceTargetUnsafe::RawHandle { raw_display_handle: rdh, raw_window_handle: rwh };
    match unsafe { instance.create_surface_unsafe(target) } {
        Ok(s) => Some(s),
        Err(_) => None,
    }
}

/// Create a wgpu context. Returns null if no adapter/device is available
/// (the C side should then fall back to the legacy D3D9 path).
///
/// `hwnd != 0` selects an adapter compatible with that window's surface so the
/// surface/adapter backend pair always matches (Vulkan vs GL on Win32).
pub fn init(hwnd: isize) -> Option<GpuInit> {
    let instance = Instance::new(&InstanceDescriptor {
        backends: Backends::VULKAN | Backends::GL,
        flags: InstanceFlags::empty(),
        ..Default::default()
    });
    let surface = raw_surface(&instance, hwnd);
    // request_adapter with compatible_surface filters adapters that actually support
    // this surface's backend (Vulkan/GL may disagree under Wine). If the winner lacks
    // COPY_DST on the swapchain, the renderer falls back to a blit render pass.
    let adapter = pollster::block_on(instance.request_adapter(&RequestAdapterOptions {
        power_preference: PowerPreference::LowPower,
        force_fallback_adapter: false,
        compatible_surface: surface.as_ref(),
    }))
    .ok()?;
    let (device, queue) = pollster::block_on(adapter.request_device(&Default::default())).ok()?;
    Some(GpuInit {
        ctx: RgContext {
            instance,
            adapter,
            device,
            queue,
        },
        surface,
    })
}

pub fn create_texture(ctx: &RgContext, w: u32, h: u32) -> Option<RgTexture> {
    if w == 0 || h == 0 {
        return None;
    }
    let texture = ctx.device.create_texture(&TextureDescriptor {
        label: Some("raindrop d7surf"),
        size: Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: TextureDimension::D2,
        // A8R8G8B8 byte order == BGRA in memory == wgpu Bgra8Unorm
        format: TextureFormat::Bgra8Unorm,
        usage: TextureUsages::TEXTURE_BINDING | TextureUsages::COPY_DST,
        view_formats: &[],
    });
    let view = texture.create_view(&TextureViewDescriptor::default());
    Some(RgTexture { texture, view, width: w, height: h })
}

/// Create a render-target-capable texture (offscreen scene / D3D9 backbuffer).
pub fn create_render_target(ctx: &RgContext, w: u32, h: u32) -> Option<RgTexture> {
    if w == 0 || h == 0 {
        return None;
    }
    let texture = ctx.device.create_texture(&TextureDescriptor {
        label: Some("raindrop render target"),
        size: Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        mip_level_count: 1,
        sample_count: 1,
        dimension: TextureDimension::D2,
        format: TextureFormat::Bgra8Unorm,
        usage: TextureUsages::TEXTURE_BINDING | TextureUsages::COPY_DST | TextureUsages::COPY_SRC | TextureUsages::RENDER_ATTACHMENT,
        view_formats: &[],
    });
    let view = texture.create_view(&TextureViewDescriptor::default());
    Some(RgTexture { texture, view, width: w, height: h })
}

/// Upload a region of ARGB8888 rows (dst_pitch in *pixels*) into the texture.
/// `data` points at `h` rows of `pitch` u32 pixels; the region `x`,`y` is `w` wide.
pub fn upload(
    ctx: &RgContext,
    tex: &RgTexture,
    x: u32,
    y: u32,
    w: u32,
    h: u32,
    data: &[u8],
    pitch_pixels: u32,
) {
    assert_eq!(tex.texture.format(), TextureFormat::Bgra8Unorm);
    let bytes_per_row = pitch_pixels * 4;
    let region = TexelCopyTextureInfo {
        texture: &tex.texture,
        mip_level: 0,
        origin: Origin3d { x, y, z: 0 },
        aspect: TextureAspect::All,
    };
    let layout = TexelCopyBufferLayout {
        offset: 0,
        bytes_per_row: Some(bytes_per_row),
        rows_per_image: Some(h.max(1)),
    };
    ctx.queue.write_texture(
        region,
        data,
        layout,
        Extent3d { width: w, height: h, depth_or_array_layers: 1 },
    );
}

pub fn poll(ctx: &RgContext) {
    ctx.queue.submit([]);
    let _ = ctx.device.poll(wgpu::PollType::Poll);
}

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

/// # Safety
/// `ctx` must be a pointer returned by `raindrop_gpu_init`, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn raindrop_gpu_init() -> *mut RgContext {
    // no HWND here (used by the C ddraw shim); surface-free adapter selection
    match init(0) {
        Some(g) => Box::into_raw(Box::new(g.ctx)),
        None => std::ptr::null_mut(),
    }
}

/// # Safety
/// `ctx` must be a pointer returned by `raindrop_gpu_init`, or null; must not be used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn raindrop_gpu_destroy(ctx: *mut RgContext) {
    unsafe {
        if !ctx.is_null() {
            drop(Box::from_raw(ctx));
        }
    }
}

/// # Safety
/// `ctx` must be valid; returns null when texture creation fails.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn raindrop_texture_create(
    ctx: *mut RgContext,
    w: u32,
    h: u32,
) -> *mut RgTexture {
    unsafe {
        if ctx.is_null() {
            return std::ptr::null_mut();
        }
        match create_texture(&*ctx, w, h) {
            Some(t) => Box::into_raw(Box::new(t)),
            None => std::ptr::null_mut(),
        }
    }
}

/// # Safety
/// `tex` must be a pointer returned by `raindrop_texture_create`; must not be used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn raindrop_texture_destroy(tex: *mut RgTexture) {
    unsafe {
        if !tex.is_null() {
            drop(Box::from_raw(tex));
        }
    }
}

/// # Safety
/// `ctx`/`tex` valid; `data` must contain `h` rows of `pitch` u32 pixels of converted ARGB8888.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn raindrop_texture_upload(
    ctx: *mut RgContext,
    tex: *mut RgTexture,
    x: u32,
    y: u32,
    w: u32,
    h: u32,
    data: *const u8,
    pitch_pixels: u32,
) {
    unsafe {
        if ctx.is_null() || tex.is_null() || data.is_null() || w == 0 || h == 0 {
            return;
        }
        let len = (h as usize - 1) * (pitch_pixels as usize * 4) + w as usize * 4;
        let slice = std::slice::from_raw_parts(data, len);
        upload(&*ctx, &*tex, x, y, w, h, slice, pitch_pixels);
    }
}

/// # Safety
/// `ctx` must be valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn raindrop_gpu_poll(ctx: *mut RgContext) {
    unsafe {
        if !ctx.is_null() {
            poll(&*ctx);
        }
    }
}
