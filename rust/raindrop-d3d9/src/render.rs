// wgpu renderer behind the emulated IDirect3DDevice9.
//
// Frame model: draws go into an offscreen "scene" texture (the D3D9 backbuffer);
// Present() copies it to the swapchain frame and presents. Clear() issues an
// immediate clear pass. Pipelines are cached per (blend, alpha-test) key.

use crate::com::*;
use crate::d3d9::*;
use crate::types_gen::*;
use crate::vtbl::{Surface, Texture};
use raindrop_gpu::gpu::{RgContext, RgTexture};
use std::collections::HashMap;
use std::sync::Arc;

const WGSL: &str = r#"
struct Vp { x: f32, y: f32, w: f32, h: f32, alpha_ref: f32, alpha_func: f32, flags: f32, pad: f32 }
@group(0) @binding(0) var<uniform> vp: Vp;
@group(0) @binding(1) var tex: texture_2d<f32>;
@group(0) @binding(2) var samp: sampler;

struct VsOut {
    @builtin(position) pos: vec4<f32>,
    @location(0) color: vec4<f32>,
    @location(1) uv: vec2<f32>,
}

@vertex
fn vs(@location(0) xywrhw: vec4<f32>, @location(1) col: vec4<f32>, @location(2) uv: vec2<f32>) -> VsOut {
    let rhw = max(xywrhw.w, 1e-9);
    let px = 0.5; // D3D9 pixel-center offset
    let x = ((xywrhw.x + px) - vp.x) / (vp.w * 0.5) - 1.0;
    let y = 1.0 - ((xywrhw.y + px) - vp.y) / (vp.h * 0.5);
    var out: VsOut;
    out.pos = vec4<f32>(x, y, xywrhw.z, 1.0 / rhw);
    out.color = col;
    out.uv = uv;
    return out;
}

@fragment
fn fs(in: VsOut) -> @location(0) vec4<f32> {
    let t = textureSample(tex, samp, in.uv);
    var c = t * in.color;
    // alpha test (flags bit0 = enabled); compareFunc is encoded 1..8 like D3DCMP
    if (vp.flags >= 1.5) {
        var keep = false;
        let a = c.a;
        let r = vp.alpha_ref;
        if (vp.alpha_func < 1.5) { keep = false; }
        else if (vp.alpha_func < 2.5) { keep = a < r; }
        else if (vp.alpha_func < 3.5) { keep = a == r; }
        else if (vp.alpha_func < 4.5) { keep = a <= r; }
        else if (vp.alpha_func < 5.5) { keep = a > r; }
        else if (vp.alpha_func < 6.5) { keep = a != r; }
        else if (vp.alpha_func < 7.5) { keep = a >= r; }
        else { keep = true; }
        if (!keep) { discard; }
    }
    return c;
}
"#;

#[derive(Clone, Copy, PartialEq, Eq, Hash)]
struct PipeKey {
    blend: u64,
    alphatest: u64,
}

pub struct Renderer {
    pub ctx: Arc<RgContext>,
    surface: Option<wgpu::Surface<'static>>,
    config: wgpu::SurfaceConfiguration,
    interval: u32,
    scene: Option<RgTexture>,
    scene_w: u32,
    scene_h: u32,
    pipelines: HashMap<PipeKey, wgpu::RenderPipeline>,
    dummy: Option<RgTexture>,
    vp: D3DVIEWPORT9,
    clear_color: Option<(u32, u32)>, // (flags, color)
}

impl Renderer {
    pub fn new(ctx: Arc<RgContext>, hwnd: isize, w: u32, h: u32) -> Result<Renderer, Hr> {
        let surface = unsafe { create_surface(&ctx, hwnd) };
        let interval = D3DPRESENT_INTERVAL_ONE;
        let config = surface_config(&ctx, &surface, w, h, interval);
        if let Some(s) = &surface {
            s.configure(ctx.device(), &config);
        }
        let mut r = Renderer {
            ctx,
            surface,
            config,
            interval,
            scene: None,
            scene_w: 0,
            scene_h: 0,
            pipelines: HashMap::new(),
            dummy: None,
            vp: D3DVIEWPORT9 { X: 0, Y: 0, Width: w, Height: h, MinZ: 0.0, MaxZ: 1.0 },
            clear_color: None,
        };
        r.ensure_scene(w, h);
        r.make_dummy();
        Ok(r)
    }

    pub fn ctx(&self) -> &RgContext {
        &self.ctx
    }

    fn ensure_scene(&mut self, w: u32, h: u32) {
        if self.scene_w == w && self.scene_h == h && self.scene.is_some() {
            return;
        }
        let gpu = raindrop_gpu::gpu::create_texture(&self.ctx, w, h)
            .expect("scene texture creation failed");
        // add RENDER_ATTACHMENT for clear passes
        self.scene = Some(gpu);
        self.scene_w = w;
        self.scene_h = h;
    }

    pub fn reconfigure(&mut self, w: u32, h: u32, interval: u32) {
        self.interval = interval;
        self.config.width = w;
        self.config.height = h;
        self.config.present_mode = present_mode(interval);
        if let Some(s) = &self.surface {
            s.configure(self.ctx.device(), &self.config);
        }
        self.ensure_scene(w, h);
        self.vp = D3DVIEWPORT9 { X: 0, Y: 0, Width: w, Height: h, MinZ: 0.0, MaxZ: 1.0 };
    }

    pub fn set_viewport(&mut self, vp: &D3DVIEWPORT9) {
        self.vp = *vp;
    }

    pub fn set_rs(&mut self, state: u32, value: u32) {
        if state == D3DRS_ALPHATESTENABLE {
            // picked up per-draw via rs array
            let _ = value;
        }
    }

    pub fn begin_scene(&mut self) {}

    pub fn clear(&mut self, flags: u32, color: u32) {
        if flags & D3DCLEAR_TARGET != 0 {
            self.clear_color = Some((flags, color));
        }
    }

    fn pipeline(
        &mut self,
        rs: &[u32; 256],
        format: wgpu::TextureFormat,
    ) -> wgpu::RenderPipeline {
        let blend_enabled = rs[D3DRS_ALPHABLENDENABLE as usize] != 0;
        let src = rs[D3DRS_SRCBLEND as usize];
        let dst = rs[D3DRS_DESTBLEND as usize];
        let op = rs[D3DRS_BLENDOP as usize];
        let at = rs[D3DRS_ALPHATESTENABLE as usize] != 0;
        let afunc = rs[D3DRS_ALPHAFUNC as usize];
        let aref = rs[D3DRS_ALPHAREF as usize];
        let key = PipeKey {
            blend: (blend_enabled as u64) | ((src as u64) << 8) | ((dst as u64) << 16) | ((op as u64) << 24),
            alphatest: (at as u64) | ((afunc as u64) << 8) | ((aref as u64) << 16),
        };
        let module = self.ctx.device().create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("raindrop d9"),
            source: wgpu::ShaderSource::Wgsl(WGSL.into()),
        });
        let bpl = wgpu::BlendComponent {
            src_factor: blend_factor(src, true),
            dst_factor: blend_factor(dst, false),
            operation: blend_op(op),
        };
        let alpha_component = wgpu::BlendComponent {
            src_factor: blend_factor(src, true),
            dst_factor: blend_factor(dst, false),
            operation: blend_op(op),
        };
        let targets = [Some(wgpu::ColorTargetState {
            format,
            blend: Some(wgpu::BlendState { color: bpl, alpha: alpha_component }),
            write_mask: wgpu::ColorWrites::ALL,
        })];
        let layout = self.ctx.device().create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: Some("raindrop d9 layout"),
            bind_group_layouts: &[&self.bind_layout()],
            push_constant_ranges: &[],
        });
        let pipeline = self
            .ctx
            .device()
            .create_render_pipeline(&wgpu::RenderPipelineDescriptor {
                label: Some("raindrop d9 pipe"),
                layout: Some(&layout),
                vertex: wgpu::VertexState {
                    module: &module,
                    entry_point: Some("vs"),
                    buffers: &[vertex_layout()],
                    compilation_options: Default::default(),
                },
                fragment: Some(wgpu::FragmentState {
                    module: &module,
                    entry_point: Some("fs"),
                    targets: &targets,
                    compilation_options: Default::default(),
                }),
                primitive: wgpu::PrimitiveState {
                    topology: wgpu::PrimitiveTopology::TriangleList,
                    cull_mode: None,
                    ..Default::default()
                },
                depth_stencil: None,
                multisample: Default::default(),
                multiview: None,
                cache: None,
            });
        self.pipelines.insert(key, pipeline.clone());
        pipeline
    }

    fn bind_layout(&self) -> wgpu::BindGroupLayout {
        self.ctx.device().create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
            label: Some("raindrop d9 bgl"),
            entries: &[
                wgpu::BindGroupLayoutEntry {
                    binding: 0,
                    visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                    ty: wgpu::BindingType::Buffer {
                        ty: wgpu::BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: Some(wgpu::BufferSize::new(8 * 4).unwrap()),
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 1,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Texture {
                        sample_type: wgpu::TextureSampleType::Float { filterable: true },
                        view_dimension: wgpu::TextureViewDimension::D2,
                        multisampled: false,
                    },
                    count: None,
                },
                wgpu::BindGroupLayoutEntry {
                    binding: 2,
                    visibility: wgpu::ShaderStages::FRAGMENT,
                    ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                    count: None,
                },
            ],
        })
    }

    #[allow(clippy::too_many_arguments)]
    pub fn draw(
        &mut self,
        prim: u32,
        fvf: u32,
        verts: &[u8],
        stride: u32,
        nverts: usize,
        _indices_unused: *const u16,
        _nidx_unused: usize,
        textures: &[Option<*mut crate::com::ComObj>; 8],
        rs: &[u32; 256],
        tss: &[[u32; 33]; 8],
        smp: &[[u32; 16]; 8],
        vp: &D3DVIEWPORT9,
    ) -> Hr {
        if fvf & D3DFVF_POSITION_MASK != D3DFVF_XYZRHW {
            warn_once(
                crate::com::Tag::Device,
                "only XYZRHW fixed-function drawing is emulated",
                D3DERR_INVALIDCALL,
            );
            return D3DERR_INVALIDCALL;
        }
        let Some(scene) = &self.scene else {
            return D3DERR_INVALIDCALL;
        };
        let colorop = tss[0][D3DTSS_COLOROP as usize];
        let alphaop = tss[0][D3DTSS_ALPHAOP as usize];
        let _ = (colorop, alphaop);

        // build GPU vertices
        let mut gpu = Vec::with_capacity(nverts * 10 * 4);
        let has_diffuse = fvf & D3DFVF_DIFFUSE != 0;
        let has_tex = fvf & D3DFVF_TEX1 != 0;
        let cd = has_diffuse as usize * 4;
        let cs = has_diffuse as usize * 4 + (fvf & D3DFVF_SPECULAR != 0) as usize * 4;
        let ct = cs;
        let stride = stride as usize;
        for v in 0..nverts {
            let b = &verts[v * stride..];
            let x = f32::from_le_bytes([b[0], b[1], b[2], b[3]]);
            let y = f32::from_le_bytes([b[4], b[5], b[6], b[7]]);
            let z = f32::from_le_bytes([b[8], b[9], b[10], b[11]]);
            let rhw = f32::from_le_bytes([b[12], b[13], b[14], b[15]]);
            let (r, g, bl, a) = if has_diffuse {
                let c = u32::from_le_bytes([b[16], b[17], b[18], b[19]]);
                (
                    ((c >> 16) & 0xFF) as f32 / 255.0,
                    ((c >> 8) & 0xFF) as f32 / 255.0,
                    (c & 0xFF) as f32 / 255.0,
                    ((c >> 24) & 0xFF) as f32 / 255.0,
                )
            } else {
                (1.0, 1.0, 1.0, 1.0)
            };
            let (u, vv) = if has_tex {
                let off = ct;
                (
                    f32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]]),
                    f32::from_le_bytes([b[off + 4], b[off + 5], b[off + 6], b[off + 7]]),
                )
            } else {
                (0.0, 0.0)
            };
            gpu.extend_from_slice(&x.to_le_bytes());
            gpu.extend_from_slice(&y.to_le_bytes());
            gpu.extend_from_slice(&z.to_le_bytes());
            gpu.extend_from_slice(&rhw.to_le_bytes());
            gpu.extend_from_slice(&r.to_le_bytes());
            gpu.extend_from_slice(&g.to_le_bytes());
            gpu.extend_from_slice(&bl.to_le_bytes());
            gpu.extend_from_slice(&a.to_le_bytes());
            gpu.extend_from_slice(&u.to_le_bytes());
            gpu.extend_from_slice(&vv.to_le_bytes());
        }
        let vbuf = self.ctx.device().create_buffer(&wgpu::BufferDescriptor {
            label: Some("raindrop d9 verts"),
            size: gpu.len() as u64,
            usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        self.ctx.queue().write_buffer(&vbuf, 0, &gpu);

        // viewport uniforms
        let at = rs[D3DRS_ALPHATESTENABLE as usize] != 0;
        let uni = build_uniform(vp, at, rs[D3DRS_ALPHAFUNC as usize], rs[D3DRS_ALPHAREF as usize]);
        let ubuf = self.ctx.device().create_buffer(&wgpu::BufferDescriptor {
            label: Some("raindrop d9 uni"),
            size: 32,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        self.ctx.queue().write_buffer(&ubuf, 0, &uni);

        // bind texture (stage 0) or dummy
        let texobj = textures[0].map(|t| unsafe { &*((**(&t)).data as *mut Texture) });
        let bg;
        let filtering = smp[0][D3DSAMP_MAGFILTER as usize] == 2 || smp[0][D3DSAMP_MINFILTER as usize] == 2;
        match texobj {
            Some(t) if t.gpu.is_some() => {
                let v = t.gpu.as_ref().unwrap().create_view(&wgpu::TextureViewDescriptor::default());
                bg = self.ctx.device().create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("raindrop d9 bind"),
                    layout: &self.bind_layout(),
                    entries: &[
                        wgpu::BindGroupEntry { binding: 0, resource: ubuf.as_entire_binding() },
                        wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(&v) },
                        wgpu::BindGroupEntry {
                            binding: 2,
                            resource: wgpu::BindingResource::Sampler(&self.sampler(filtering)),
                        },
                    ],
                });
            }
            _ => {
                let dummy = self.dummy_texture();
                let v = dummy.texture().create_view(&wgpu::TextureViewDescriptor::default());
                bg = self.ctx.device().create_bind_group(&wgpu::BindGroupDescriptor {
                    label: Some("raindrop d9 bind dummy"),
                    layout: &self.bind_layout(),
                    entries: &[
                        wgpu::BindGroupEntry { binding: 0, resource: ubuf.as_entire_binding() },
                        wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(&v) },
                        wgpu::BindGroupEntry {
                            binding: 2,
                            resource: wgpu::BindingResource::Sampler(&self.sampler(filtering)),
                        },
                    ],
                });
            }
        }

        // encode the draw
        let clear = self.clear_color.take();
        let scene_view = scene.texture().create_view(&wgpu::TextureViewDescriptor::default());
        let mut enc = self.ctx.device().create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("raindrop d9 enc"),
        });
        {
            let load = match clear {
                Some((_, color)) => wgpu::LoadOp::Clear(wgpu::Color {
                    r: ((color >> 16) & 0xFF) as f64 / 255.0,
                    g: ((color >> 8) & 0xFF) as f64 / 255.0,
                    b: (color & 0xFF) as f64 / 255.0,
                    a: ((color >> 24) & 0xFF) as f64 / 255.0,
                }),
                None => wgpu::LoadOp::Load,
            };
            let mut pass = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("raindrop d9 pass"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &scene_view,
                    resolve_target: None,
                    ops: wgpu::Operations { load, store: wgpu::StoreOp::Store },
                    depth_slice: None,
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            let format = scene.texture().format();
            let pipe = self.pipeline(rs, format);
            pass.set_pipeline(&pipe);
            pass.set_bind_group(0, &bg, &[]);
            pass.set_vertex_buffer(0, vbuf.slice(..));
            pass.draw(0..nverts as u32, 0..1);
        }
        self.ctx.queue().submit(Some(enc.finish()));
        S_OK
    }

    pub fn draw_indexed(
        &mut self,
        prim: u32,
        fvf: u32,
        verts: &[u8],
        stride: u32,
        indices: &[u16],
        textures: &[Option<*mut crate::com::ComObj>; 8],
        rs: &[u32; 256],
        tss: &[[u32; 33]; 8],
        smp: &[[u32; 16]; 8],
        vp: &D3DVIEWPORT9,
    ) -> Hr {
        // de-indexed expand: reuse the indexed path by expanding in place
        let stride = stride as usize;
        let mut expanded = Vec::with_capacity(indices.len() * stride);
        for &i in indices {
            let s = i as usize * stride;
            if s + stride <= verts.len() {
                expanded.extend_from_slice(&verts[s..s + stride]);
            }
        }
        self.draw(
            prim, fvf, &expanded, stride as u32, indices.len(), core::ptr::null(), 0, textures, rs, tss,
            smp, vp,
        )
    }

    fn sampler(&self, filtering: bool) -> wgpu::Sampler {
        self.ctx.device().create_sampler(&wgpu::SamplerDescriptor {
            label: Some("raindrop d9 samp"),
            mag_filter: if filtering { wgpu::FilterMode::Linear } else { wgpu::FilterMode::Nearest },
            min_filter: if filtering { wgpu::FilterMode::Linear } else { wgpu::FilterMode::Nearest },
            mipmap_filter: wgpu::FilterMode::Nearest,
            address_mode_u: wgpu::AddressMode::Repeat,
            address_mode_v: wgpu::AddressMode::Repeat,
            ..Default::default()
        })
    }

    fn make_dummy(&mut self) {
        if self.dummy.is_none() {
            let t = raindrop_gpu::gpu::create_texture(&self.ctx, 1, 1).unwrap();
            self.dummy = Some(t);
            raindrop_gpu::gpu::upload(&self.ctx, self.dummy.as_ref().unwrap(), 0, 0, 1, 1, &[0xFFu8, 0xFF, 0xFF, 0xFF], 1);
        }
    }

    fn dummy_texture(&self) -> &RgTexture {
        self.dummy.as_ref().unwrap()
    }

    pub fn present(&mut self) -> Hr {
        let Some(surface) = &self.surface else {
            return D3DERR_DEVICELOST;
        };
        let Some(scene) = &self.scene else {
            return D3DERR_DEVICELOST;
        };
        let frame = match surface.get_current_texture() {
            Ok(f) => f,
            Err(e) => {
                let _ = e;
                surface.configure(self.ctx.device(), &self.config);
                match surface.get_current_texture() {
                    Ok(f) => f,
                    Err(_) => return D3DERR_DEVICELOST,
                }
            }
        };
        let mut enc = self.ctx.device().create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("raindrop d9 present"),
        });
        enc.copy_texture_to_texture(
            scene.texture().as_image_copy(),
            wgpu::TexelCopyTextureInfo {
                texture: &frame.texture,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            wgpu::Extent3d { width: self.scene_w, height: self.scene_h, depth_or_array_layers: 1 },
        );
        self.ctx.queue().submit(Some(enc.finish()));
        frame.present();
        S_OK
    }

    pub fn flush_textures(&mut self, textures: &[Option<*mut crate::com::ComObj>; 8]) {
        for t in textures.iter().flatten() {
            let tex = unsafe { &mut *((**t).data as *mut Texture) };
            if tex.levels_sys[0].dirty {
                let ctx = self.ctx.clone();
                crate::render::upload_texture(&ctx, *t, tex);
            }
        }
    }
}

fn build_uniform(vp: &D3DVIEWPORT9, alphatest: bool, afunc: u32, aref: u32) -> [u8; 32] {
    let mut u = [0u8; 32];
    put(&mut u, 0, vp.X as f32);
    put(&mut u, 4, vp.Y as f32);
    put(&mut u, 8, vp.Width as f32);
    put(&mut u, 12, vp.Height as f32);
    put(&mut u, 16, (aref & 0xFF) as f32 / 255.0);
    put(&mut u, 20, afunc as f32);
    put(&mut u, 24, if alphatest { 1.0 } else { 0.0 });
    u
}

fn put(buf: &mut [u8], off: usize, v: f32) {
    buf[off..off + 4].copy_from_slice(&v.to_le_bytes());
}

fn blend_factor(v: u32, src: bool) -> wgpu::BlendFactor {
    match v {
        D3DBLEND_ZERO => wgpu::BlendFactor::Zero,
        D3DBLEND_ONE => wgpu::BlendFactor::One,
        D3DBLEND_SRCCOLOR => wgpu::BlendFactor::Src,
        D3DBLEND_INVSRCCOLOR => wgpu::BlendFactor::OneMinusSrc,
        D3DBLEND_SRCALPHA => wgpu::BlendFactor::SrcAlpha,
        D3DBLEND_INVSRCALPHA => wgpu::BlendFactor::OneMinusSrcAlpha,
        D3DBLEND_DESTALPHA => wgpu::BlendFactor::DstAlpha,
        D3DBLEND_INVDESTALPHA => wgpu::BlendFactor::OneMinusDstAlpha,
        D3DBLEND_DESTCOLOR => wgpu::BlendFactor::Dst,
        D3DBLEND_INVDESTCOLOR => wgpu::BlendFactor::OneMinusDst,
        D3DBLEND_SRCALPHASAT => {
            let _ = src;
            wgpu::BlendFactor::SrcAlphaSaturated
        }
        _ => wgpu::BlendFactor::One,
    }
}

fn blend_op(v: u32) -> wgpu::BlendOperation {
    // D3DBLENDOP_ADD=1, SUBTRACT=2, REVSUBTRACT=3, MIN=4, MAX=5
    match v {
        1 => wgpu::BlendOperation::Add,
        2 => wgpu::BlendOperation::Subtract,
        3 => wgpu::BlendOperation::ReverseSubtract,
        4 => wgpu::BlendOperation::Min,
        5 => wgpu::BlendOperation::Max,
        _ => wgpu::BlendOperation::Add,
    }
}

fn present_mode(interval: u32) -> wgpu::PresentMode {
    if interval == D3DPRESENT_INTERVAL_IMMEDIATE || interval == D3DPRESENT_INTERVAL_DEFAULT {
        wgpu::PresentMode::Immediate
    } else {
        wgpu::PresentMode::Fifo
    }
}

fn vertex_layout() -> wgpu::VertexBufferLayout<'static> {
    wgpu::VertexBufferLayout {
        array_stride: 10 * 4,
        step_mode: wgpu::VertexStepMode::Vertex,
        attributes: &[
            wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x4, offset: 0, shader_location: 0 },
            wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x4, offset: 16, shader_location: 1 },
            wgpu::VertexAttribute { format: wgpu::VertexFormat::Float32x2, offset: 32, shader_location: 2 },
        ],
    }
}

fn surface_config(
    ctx: &RgContext,
    surface: &Option<wgpu::Surface<'static>>,
    w: u32,
    h: u32,
    interval: u32,
) -> wgpu::SurfaceConfiguration {
    let caps = surface
        .as_ref()
        .map(|s| s.get_capabilities(ctx.adapter()))
        .unwrap_or_else(|| wgpu::SurfaceCapabilities::default());
    wgpu::SurfaceConfiguration {
        usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_DST,
        format: wgpu::TextureFormat::Bgra8Unorm,
        width: w.max(1),
        height: h.max(1),
        present_mode: present_mode(interval),
        alpha_mode: caps.alpha_modes.first().copied().unwrap_or(wgpu::CompositeAlphaMode::Opaque),
        view_formats: vec![],
        desired_maximum_frame_latency: 2,
    }
}

unsafe fn create_surface(ctx: &RgContext, hwnd: isize) -> Option<wgpu::Surface<'static>> {
    use raw_window_handle::{RawDisplayHandle, RawWindowHandle, WindowsDisplayHandle, Win32WindowHandle};
    use wgpu::SurfaceTargetUnsafe;
    use core::num::NonZeroIsize;
    let mut wh = Win32WindowHandle::new(NonZeroIsize::new(hwnd)?);
    wh.hinstance = None;
    let rdh = RawDisplayHandle::Windows(WindowsDisplayHandle::new());
    let rwh = RawWindowHandle::Win32(wh);
    let target = SurfaceTargetUnsafe::RawHandle { raw_display_handle: rdh, raw_window_handle: rwh };
    let s = ctx.instance().create_surface_unsafe(target).ok()?;
    Some(s)
}

// ---- uploads ----------------------------------------------------------------

/// Create the GPU mirror for a D3D9 texture (BGRA8 / 565 family).
pub fn create_gpu_texture(
    ctx: &RgContext,
    w: u32,
    h: u32,
    fmt: u32,
    usage: u32,
) -> Option<RgTexture> {
    let _ = usage;
    raindrop_gpu::gpu::create_texture(ctx, w, h)
}

/// Upload a dirty texture's level-0 sysmem into the GPU texture.
pub fn upload_texture(ctx: &RgContext, _obj: *mut crate::com::ComObj, t: &mut Texture) {
    let Some(gpu) = &t.gpu else { return };
    let l = &mut t.levels_sys[0];
    if !l.dirty {
        return;
    }
    l.dirty = false;
    let bpp = bytes_per_pixel(t.fmt) as usize;
    let mut data = core::mem::take(&mut l.data);
    if bpp == 4 {
        if t.fmt == D3DFMT_X8R8G8B8 {
            for px in data.chunks_exact_mut(4) {
                px[3] = 0xFF; // X8 must read as opaque
            }
        }
    } else if bpp == 2 {
        // 16bpp source -> BGRA8 rows via the fearless_simd kernels
        let code = match t.fmt {
            D3DFMT_R5G6B5 => 0,
            D3DFMT_X1R5G5B5 => 1,
            D3DFMT_A1R5G5B5 => 2,
            D3DFMT_A4R4G4B4 => 3,
            _ => 0,
        };
        let mut out = vec![0u8; t.w as usize * t.h as usize * 4];
        for y in 0..t.h as usize {
            let row = &data[y * l.pitch..];
            let dst = &mut out[y * t.w as usize * 4..];
            let src: Vec<u16> = row[..t.w as usize * 2]
                .chunks_exact(2)
                .map(|c| u16::from_le_bytes([c[0], c[1]]))
                .collect();
            raindrop_gpu::kernels::upload_conv16(
                bytemuck_dst(&mut dst[..t.w as usize * 4]),
                &src,
                fmt_code(code),
                None,
            );
        }
        data = out;
    } else if bpp == 1 {
        // A8 -> expand into BGRA8 (a,r=g=b=a)
        let mut out = vec![0u8; t.w as usize * t.h as usize * 4];
        for (i, &a) in data.iter().enumerate() {
            out[i * 4] = 0xFF; // b
            out[i * 4 + 1] = 0xFF; // g
            out[i * 4 + 2] = 0xFF; // r
            out[i * 4 + 3] = a;
        }
        data = out;
    }
    let region = wgpu::TexelCopyTextureInfo {
        texture: gpu,
        mip_level: 0,
        origin: wgpu::Origin3d::ZERO,
        aspect: wgpu::TextureAspect::All,
    };
    let layout = wgpu::TexelCopyBufferLayout {
        offset: 0,
        bytes_per_row: Some(t.w * 4),
        rows_per_image: Some(t.h),
    };
    ctx.queue().write_texture(
        region,
        &data,
        layout,
        wgpu::Extent3d { width: t.w, height: t.h, depth_or_array_layers: 1 },
    );
    l.data = data;
}

fn fmt_code(code: i32) -> raindrop_gpu::kernels::Fmt16 {
    use raindrop_gpu::kernels::Fmt16;
    match code {
        0 => Fmt16::Rgb565,
        1 => Fmt16::Rgb555,
        2 => Fmt16::Rgba5551,
        _ => Fmt16::Rgba4444,
    }
}

/// Reinterpret a &mut [u8] as &mut [u32] (len / 4).
fn bytemuck_dst(b: &mut [u8]) -> &mut [u32] {
    unsafe { core::slice::from_raw_parts_mut(b.as_mut_ptr() as *mut u32, b.len() / 4) }
}

/// Upload a dirty surface's sysmem (offscreen plain surfaces).
pub fn upload_surface(ctx: &RgContext, obj: *mut crate::com::ComObj, s: &mut Surface) {
    // surfaces have no gpu texture of their own; sysmem uploads are only needed
    // for UpdateSurface/ColorFill paths into render targets, which are stubbed.
    let _ = (ctx, obj, s);
}
