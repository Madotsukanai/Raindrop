// wgpu renderer behind the emulated IDirect3DDevice9.
//
// Frame model: draws are *recorded* into a FrameBatch (vertices expanded into a
// reusable Vec, one 32B uniform slot per draw). Present() replays the whole
// batch in a single render pass into the scene texture, then blits/copies it to
// the swapchain — one submit, one present per frame. Bind groups, views and
// pipelines are cached and resolved at record time, so no GPU object is
// created on the hot path.

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
    let x = ((xywrhw.x + px) - vp.x) / (max(vp.w, 1.0) * 0.5) - 1.0;
    let y = 1.0 - ((xywrhw.y + px) - vp.y) / (max(vp.h, 1.0) * 0.5);
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

/// Uniform slots are allocated at 256-byte stride: the dynamic-offset alignment
/// for uniform buffers lets every draw share one bind group.
const UNI_STRIDE: usize = 256;

/// One recorded draw: fully resolved GPU objects, replayed verbatim at Present.
struct DrawCmd {
    pipe: PipeKey,
    bg: wgpu::BindGroup,
    base_v: u32,
    n_v: u32,
    uni_off: u32,
}

/// Draws accumulated since the last Present, replayed in one render pass.
#[derive(Default)]
struct FrameBatch {
    /// GPU-format vertices (40B each), expanded at record time
    verts: Vec<u8>,
    /// uniform data, UNI_STRIDE-aligned slots
    uni: Vec<u8>,
    cmds: Vec<DrawCmd>,
}

pub struct Renderer {
    pub ctx: Arc<RgContext>,
    surface: Option<wgpu::Surface<'static>>,
    config: wgpu::SurfaceConfiguration,
    interval: u32,
    scene: Option<RgTexture>,
    scene_w: u32,
    scene_h: u32,
    /// true when the swapchain lacks COPY_DST (e.g. GL surfaces): Present blits
    /// the scene with a render pass instead of copy_texture_to_texture
    blit: bool,
    pipelines: HashMap<(PipeKey, wgpu::TextureFormat), wgpu::RenderPipeline>,
    bgl: Option<wgpu::BindGroupLayout>,
    samp_near: Option<wgpu::Sampler>,
    samp_lin: Option<wgpu::Sampler>,
    /// persistent vertex/uniform staging buffers: creating device memory per draw
    /// is the dominant per-call cost on real Vulkan drivers
    vbuf: Option<wgpu::Buffer>,
    ubuf: Option<wgpu::Buffer>,
    dummy: Option<RgTexture>,
    vp: D3DVIEWPORT9,
    clear_color: Option<(u32, u32)>, // (flags, color)
    frame: FrameBatch,
    /// cached default view of the scene texture (recreated with the scene)
    scene_view: Option<wgpu::TextureView>,
    /// cached view/bind groups of the 1x1 dummy texture
    dummy_view: Option<wgpu::TextureView>,
    dummy_bg: [Option<wgpu::BindGroup>; 2],
    /// static fullscreen-quad resources for the blit-Present path
    blit_vbuf: Option<wgpu::Buffer>,
    blit_ubuf: Option<wgpu::Buffer>,
    blit_bg: Option<wgpu::BindGroup>,
}

impl Renderer {
    pub fn new(ctx: Arc<RgContext>, surface: Option<wgpu::Surface<'static>>, w: u32, h: u32, interval: u32) -> Result<Renderer, Hr> {
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
            blit: false,
            pipelines: HashMap::new(),
            bgl: None,
            samp_near: None,
            samp_lin: None,
            vbuf: None,
            ubuf: None,
            dummy: None,
            vp: D3DVIEWPORT9 { X: 0, Y: 0, Width: w, Height: h, MinZ: 0.0, MaxZ: 1.0 },
            clear_color: None,
            frame: FrameBatch::default(),
            scene_view: None,
            dummy_view: None,
            dummy_bg: [None, None],
            blit_vbuf: None,
            blit_ubuf: None,
            blit_bg: None,
        };
        // static resources: bind group layout + the two samplers are used by every
        // draw; creating them per call is a measurable cost on software backends
        r.bgl = Some(create_bind_group_layout(&r.ctx));
        r.samp_near = Some(create_sampler(&r.ctx, false));
        r.samp_lin = Some(create_sampler(&r.ctx, true));
        // vertex buffer grows on demand; uniforms sit at 256B slots with dynamic
        // offsets so one bind group serves every draw
        r.vbuf = Some(r.ctx.device().create_buffer(&wgpu::BufferDescriptor {
            label: Some("raindrop d9 vbuf pool"),
            size: 1 << 20,
            usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        }));
        r.ubuf = Some(r.ctx.device().create_buffer(&wgpu::BufferDescriptor {
            label: Some("raindrop d9 ubuf pool"),
            size: 1024 * UNI_STRIDE as u64,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        }));
        r.make_dummy();
        r.ensure_scene(w, h);
        if let Some(s) = &r.surface {
            // drop-in: only enable blit when the copy path is unsupported
            r.blit = !s.get_capabilities(&r.ctx.adapter()).usages.contains(wgpu::TextureUsages::COPY_DST);
        }
        Ok(r)
    }

    pub fn ctx(&self) -> &RgContext {
        &self.ctx
    }

    fn ensure_scene(&mut self, w: u32, h: u32) {
        if self.scene_w == w && self.scene_h == h && self.scene.is_some() {
            return;
        }
        let gpu = raindrop_gpu::gpu::create_render_target(&self.ctx, w, h)
            .expect("scene texture creation failed");
        self.scene = Some(gpu);
        self.scene_w = w;
        self.scene_h = h;
        self.scene_view = Some(
            self.scene
                .as_ref()
                .unwrap()
                .texture()
                .create_view(&wgpu::TextureViewDescriptor::default()),
        );
        self.setup_blit();
    }

    /// Static fullscreen-quad resources for the blit-Present path. Written once
    /// per scene size; previously rebuilt (two buffers + writes + a bind group)
    /// on every Present.
    fn setup_blit(&mut self) {
        let (w, h) = (self.scene_w, self.scene_h);
        let v: [f32; 60] = [
            0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0,
            w as f32, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.0,
            0.0, h as f32, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.0, 1.0,
            0.0, h as f32, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.0, 1.0,
            w as f32, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.0,
            w as f32, h as f32, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0,
        ];
        let vbytes: &[u8] = unsafe { core::slice::from_raw_parts(v.as_ptr().cast::<u8>(), v.len() * 4) };
        let vbuf = self.ctx.device().create_buffer(&wgpu::BufferDescriptor {
            label: Some("raindrop d9 blit verts"),
            size: 60 * 4,
            usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        self.ctx.queue().write_buffer(&vbuf, 0, vbytes);
        let ubuf = self.ctx.device().create_buffer(&wgpu::BufferDescriptor {
            label: Some("raindrop d9 blit uni"),
            size: 32,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        let uni = build_uniform(
            &D3DVIEWPORT9 { X: 0, Y: 0, Width: w, Height: h, MinZ: 0.0, MaxZ: 1.0 },
            false, 0, 0,
        );
        self.ctx.queue().write_buffer(&ubuf, 0, &uni);
        let scene_view = self.scene_view.clone().unwrap();
        let bg = self.ctx.device().create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("raindrop d9 blit bind"),
            layout: &self.bind_layout(),
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                        buffer: &ubuf,
                        offset: 0,
                        size: wgpu::BufferSize::new(32),
                    }),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::TextureView(&scene_view),
                },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::Sampler(&self.samp_near.clone().unwrap()),
                },
            ],
        });
        self.blit_vbuf = Some(vbuf);
        self.blit_ubuf = Some(ubuf);
        self.blit_bg = Some(bg);
    }

    pub fn reconfigure(&mut self, w: u32, h: u32, interval: u32) {
        self.interval = interval;
        self.config.width = w;
        self.config.height = h;
        self.config.present_mode = if let Some(s) = &self.surface {
            let caps = s.get_capabilities(&self.ctx.adapter());
            present_mode(interval, Some(&caps))
        } else {
            present_mode(interval, None)
        };
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

    fn pipeline_for(&mut self, key: PipeKey, format: wgpu::TextureFormat) -> wgpu::RenderPipeline {
        // early-out before shader/module/layout work; pipelines are the expensive
        // resource on software backends (llvmpipe compiles on every creation)
        if let Some(p) = self.pipelines.get(&(key, format)) {
            return p.clone();
        }
        let module = self.ctx.device().create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("raindrop d9"),
            source: wgpu::ShaderSource::Wgsl(WGSL.into()),
        });
        let enabled = (key.blend & 0xFF) != 0;
        let src = ((key.blend >> 8) & 0xFF) as u32;
        let dst = ((key.blend >> 16) & 0xFF) as u32;
        let op = ((key.blend >> 24) & 0xFF) as u32;
        let comp = || wgpu::BlendComponent {
            src_factor: blend_factor(src, true),
            dst_factor: blend_factor(dst, false),
            operation: blend_op(op),
        };
        let blend = if enabled {
            Some(wgpu::BlendState { color: comp(), alpha: comp() })
        } else {
            None // opaque replace
        };
        let targets = [Some(wgpu::ColorTargetState {
            format,
            blend,
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
        self.pipelines.insert((key, format), pipeline.clone());
        pipeline
    }

    fn pipe_key(rs: &[u32; 256]) -> PipeKey {
        let blend_enabled = rs[D3DRS_ALPHABLENDENABLE as usize] != 0;
        let src = rs[D3DRS_SRCBLEND as usize];
        let dst = rs[D3DRS_DESTBLEND as usize];
        let op = rs[D3DRS_BLENDOP as usize];
        let at = rs[D3DRS_ALPHATESTENABLE as usize] != 0;
        let afunc = rs[D3DRS_ALPHAFUNC as usize];
        let aref = rs[D3DRS_ALPHAREF as usize];
        PipeKey {
            blend: (blend_enabled as u64) | ((src as u64) << 8) | ((dst as u64) << 16) | ((op as u64) << 24),
            alphatest: (at as u64) | ((afunc as u64) << 8) | ((aref as u64) << 16),
        }
    }

    fn bind_layout(&self) -> wgpu::BindGroupLayout {
        self.bgl.clone().unwrap()
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
        if self.scene.is_none() {
            return D3DERR_INVALIDCALL;
        }
        let nverts = nverts.max(1);
        let stride = stride as usize;
        if verts.len() < nverts * stride {
            return D3DERR_INVALIDCALL;
        }
        let _ = (prim, &tss);

        let has_diffuse = fvf & D3DFVF_DIFFUSE != 0;
        let has_tex = fvf & D3DFVF_TEX1 != 0;
        let cd = has_diffuse as usize * 4;
        let cs = has_diffuse as usize * 4 + (fvf & D3DFVF_SPECULAR != 0) as usize * 4;
        let ct = cs;

        // expand into the frame's persistent vertex Vec: no allocation, no queue
        // work — GPU upload happens once, at Present
        let base_v = self.frame.verts.len() as u32;
        self.frame.verts.reserve(nverts * 40);
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
                let off = 16 + (has_diffuse as usize) * 4 + (fvf & D3DFVF_SPECULAR != 0) as usize * 4;
                (
                    f32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]]),
                    f32::from_le_bytes([b[off + 4], b[off + 5], b[off + 6], b[off + 7]]),
                )
            } else {
                (0.0, 0.0)
            };
            self.frame.verts.extend_from_slice(&x.to_le_bytes());
            self.frame.verts.extend_from_slice(&y.to_le_bytes());
            self.frame.verts.extend_from_slice(&z.to_le_bytes());
            self.frame.verts.extend_from_slice(&rhw.to_le_bytes());
            self.frame.verts.extend_from_slice(&r.to_le_bytes());
            self.frame.verts.extend_from_slice(&g.to_le_bytes());
            self.frame.verts.extend_from_slice(&bl.to_le_bytes());
            self.frame.verts.extend_from_slice(&a.to_le_bytes());
            self.frame.verts.extend_from_slice(&u.to_le_bytes());
            self.frame.verts.extend_from_slice(&vv.to_le_bytes());
        }
        self.record(textures[0], rs, smp, vp, base_v, nverts as u32);
        S_OK
    }

    /// Append the per-draw state (uniform slot, bind group, pipeline key) to the
    /// frame batch. Everything expensive is resolved once and cached.
    fn record(
        &mut self,
        tex: Option<*mut crate::com::ComObj>,
        rs: &[u32; 256],
        smp: &[[u32; 16]; 8],
        vp: &D3DVIEWPORT9,
        base_v: u32,
        n_v: u32,
    ) {
        let at = rs[D3DRS_ALPHATESTENABLE as usize] != 0;
        let uni = build_uniform(vp, at, rs[D3DRS_ALPHAFUNC as usize], rs[D3DRS_ALPHAREF as usize]);
        let uni_off = self.alloc_uniform(&uni);
        let filtering = smp[0][D3DSAMP_MAGFILTER as usize] == 2 || smp[0][D3DSAMP_MINFILTER as usize] == 2;
        let bg = self.bind_group_for(tex, filtering);
        let pipe = Self::pipe_key(rs);
        self.frame.cmds.push(DrawCmd { pipe, bg, base_v, n_v, uni_off });
    }

    /// Reserve one UNI_STRIDE-aligned uniform slot in the frame batch.
    fn alloc_uniform(&mut self, uni: &[u8; 32]) -> u32 {
        let off = self.frame.uni.len() as u32;
        self.frame.uni.extend_from_slice(uni);
        self.frame
            .uni
            .resize(((self.frame.uni.len() + UNI_STRIDE - 1) / UNI_STRIDE) * UNI_STRIDE, 0);
        off
    }

    /// Vertex/uniform buffers grow to fit the frame; recreation is rare and the
    /// old buffer is simply replaced.
    fn ensure_vbuf(&mut self, need: u64) -> wgpu::Buffer {
        if let Some(b) = &self.vbuf {
            if b.size() >= need {
                return b.clone();
            }
        }
        let size = need.max(1 << 20).next_power_of_two();
        let b = self.ctx.device().create_buffer(&wgpu::BufferDescriptor {
            label: Some("raindrop d9 vbuf pool"),
            size,
            usage: wgpu::BufferUsages::VERTEX | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        self.vbuf = Some(b.clone());
        b
    }

    fn ensure_ubuf(&mut self, need: u64) -> wgpu::Buffer {
        if let Some(b) = &self.ubuf {
            if b.size() >= need {
                return b.clone();
            }
        }
        let size = need.max(1024 * UNI_STRIDE as u64).next_power_of_two();
        let b = self.ctx.device().create_buffer(&wgpu::BufferDescriptor {
            label: Some("raindrop d9 ubuf pool"),
            size,
            usage: wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
            mapped_at_creation: false,
        });
        self.ubuf = Some(b.clone());
        b
    }

    /// Build the uniform+texture+sampler bind group.
    fn make_bg(&self, view: &wgpu::TextureView, filtering: bool, ubuf: &wgpu::Buffer) -> wgpu::BindGroup {
        self.ctx.device().create_bind_group(&wgpu::BindGroupDescriptor {
            label: Some("raindrop d9 bind"),
            layout: &self.bind_layout(),
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::Buffer(wgpu::BufferBinding {
                        buffer: ubuf,
                        offset: 0,
                        size: wgpu::BufferSize::new(32),
                    }),
                },
                wgpu::BindGroupEntry { binding: 1, resource: wgpu::BindingResource::TextureView(view) },
                wgpu::BindGroupEntry {
                    binding: 2,
                    resource: wgpu::BindingResource::Sampler(&self.sampler(filtering)),
                },
            ],
        })
    }

    /// Resolve the bind group for stage-0 texture: cached per (texture, filter)
    /// inside the Texture object, so a released texture drops its cache with it.
    fn bind_group_for(&mut self, tex: Option<*mut crate::com::ComObj>, filtering: bool) -> wgpu::BindGroup {
        let ubuf = self.ubuf.clone().unwrap();
        if let Some(p) = tex {
            let t = unsafe { &mut *((*p).data as *mut Texture) };
            if t.gpu.is_some() {
                let slot = filtering as usize;
                if let Some(bg) = &t.bg[slot] {
                    return bg.clone();
                }
                if t.view.is_none() {
                    t.view = Some(
                        t.gpu
                            .as_ref()
                            .unwrap()
                            .create_view(&wgpu::TextureViewDescriptor::default()),
                    );
                }
                let bg = self.make_bg(t.view.as_ref().unwrap(), filtering, &ubuf);
                t.bg[slot] = Some(bg.clone());
                return bg;
            }
        }
        // dummy (no texture bound or not yet uploaded)
        let slot = filtering as usize;
        if let Some(bg) = &self.dummy_bg[slot] {
            return bg.clone();
        }
        if self.dummy_view.is_none() {
            self.dummy_view = Some(
                self.dummy_texture()
                    .texture()
                    .create_view(&wgpu::TextureViewDescriptor::default()),
            );
        }
        let bg = self.make_bg(self.dummy_view.as_ref().unwrap(), filtering, &ubuf);
        self.dummy_bg[slot] = Some(bg.clone());
        bg
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
        if fvf & D3DFVF_POSITION_MASK != D3DFVF_XYZRHW {
            warn_once(
                crate::com::Tag::Device,
                "only XYZRHW fixed-function drawing is emulated",
                D3DERR_INVALIDCALL,
            );
            return D3DERR_INVALIDCALL;
        }
        if self.scene.is_none() {
            return D3DERR_INVALIDCALL;
        }
        let stride = stride as usize;
        if indices.is_empty() {
            return S_OK;
        }
        for &i in indices {
            let s = i as usize * stride;
            if s + stride > verts.len() {
                return D3DERR_INVALIDCALL;
            }
        }
        // de-index straight into the frame batch: no intermediate allocation
        let base_v = self.frame.verts.len() as u32;
        self.frame.verts.reserve(indices.len() * stride);
        for &i in indices {
            let s = i as usize * stride;
            self.frame.verts.extend_from_slice(&verts[s..s + stride]);
        }
        self.record(textures[0], rs, smp, vp, base_v, indices.len() as u32);
        let _ = prim;
        S_OK
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
        if self.scene.is_none() {
            return D3DERR_DEVICELOST;
        }
        let scene_format = self.scene.as_ref().unwrap().texture().format();
        let frame = match surface.get_current_texture() {
            Ok(f) => f,
            Err(_) => {
                surface.configure(self.ctx.device(), &self.config);
                match surface.get_current_texture() {
                    Ok(f) => f,
                    Err(_) => {
                        return D3DERR_DEVICELOST;
                    }
                }
            }
        };

        // resolve every distinct pipeline once, before any pass starts (the
        // pipeline cache needs &mut self, which the pass would hold)
        let mut pipe_keys: Vec<PipeKey> = Vec::new();
        for c in &self.frame.cmds {
            if !pipe_keys.contains(&c.pipe) {
                pipe_keys.push(c.pipe);
            }
        }
        let pipes: Vec<wgpu::RenderPipeline> =
            pipe_keys.iter().map(|k| self.pipeline_for(*k, scene_format)).collect();
        let blit_key = PipeKey { blend: 0, alphatest: 0 };
        let blit_pipe = if self.blit {
            Some(self.pipeline_for(blit_key, frame.texture.format()))
        } else {
            None
        };

        // one write_buffer per resource kind, then ONE encoder holding the whole
        // frame: scene draws + present blit/copy, single submit
        let vneed = self.frame.verts.len() as u64;
        let uneed = self.frame.uni.len() as u64;
        if vneed > 0 {
            let vbuf = self.ensure_vbuf(vneed);
            self.ctx.queue().write_buffer(&vbuf, 0, &self.frame.verts);
        }
        if uneed > 0 {
            let ubuf = self.ensure_ubuf(uneed);
            self.ctx.queue().write_buffer(&ubuf, 0, &self.frame.uni);
        }

        let vbuf = if vneed > 0 {
            Some(self.ensure_vbuf(vneed))
        } else {
            self.vbuf.clone()
        };
        let mut enc = self.ctx.device().create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("raindrop d9 frame"),
        });
        if !self.frame.cmds.is_empty() || self.clear_color.is_some() {
            let scene_view = self.scene_view.clone().unwrap();
            let clear = self.clear_color.take();
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
            // one vertex buffer serves the whole frame; draws address it by
            // vertex index (set_vertex_buffer sub-slices misbehave on some
            // backends, and this form also saves a state change per draw)
            let vb = vbuf.as_ref().unwrap();
            pass.set_vertex_buffer(0, vb.slice(..));
            for c in &self.frame.cmds {
                let idx = pipe_keys.iter().position(|k| *k == c.pipe).unwrap();
                pass.set_pipeline(&pipes[idx]);
                pass.set_bind_group(0, &c.bg, &[c.uni_off]);
                pass.draw(c.base_v / 40..c.base_v / 40 + c.n_v, 0..1);
            }
        }
        if self.blit {
            // swapchain without COPY_DST: draw the scene fullscreen onto the frame
            let frame_view = frame.texture.create_view(&wgpu::TextureViewDescriptor::default());
            let mut pass = enc.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: Some("raindrop d9 blit"),
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &frame_view,
                    resolve_target: None,
                    ops: wgpu::Operations { load: wgpu::LoadOp::Clear(wgpu::Color::BLACK), store: wgpu::StoreOp::Store },
                    depth_slice: None,
                })],
                depth_stencil_attachment: None,
                timestamp_writes: None,
                occlusion_query_set: None,
            });
            pass.set_pipeline(&blit_pipe.unwrap());
            pass.set_bind_group(0, self.blit_bg.as_ref().unwrap(), &[]);
            pass.set_vertex_buffer(0, self.blit_vbuf.as_ref().unwrap().slice(..));
            pass.draw(0..6, 0..1);
        } else {
            let scene = self.scene.as_ref().unwrap();
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
        }
        self.ctx.queue().submit(Some(enc.finish()));
        frame.present();
        // reap completed GPU work so staging memory from uploads/write_buffer
        // doesn't accumulate across frames
        let _ = self.ctx.device().poll(wgpu::PollType::Poll);
        // reset the frame batch (capacity is kept for the next frame)
        self.frame.verts.clear();
        self.frame.uni.clear();
        self.frame.cmds.clear();
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

fn present_mode(interval: u32, caps: Option<&wgpu::SurfaceCapabilities>) -> wgpu::PresentMode {
    let want_immediate =
        interval == D3DPRESENT_INTERVAL_IMMEDIATE || interval == D3DPRESENT_INTERVAL_DEFAULT;
    if want_immediate
        && caps
            .map(|c| c.present_modes.contains(&wgpu::PresentMode::Immediate))
            .unwrap_or(false)
    {
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
    let interval = if interval == D3DPRESENT_INTERVAL_ONE { interval } else { interval };
    // COPY_DST is optional: the present path can blit with a render pass when the
    // swapchain does not accept it (GL/WGL surfaces)
    let usage = if caps.usages.contains(wgpu::TextureUsages::COPY_DST) {
        wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_DST
    } else {
        wgpu::TextureUsages::RENDER_ATTACHMENT
    };
    wgpu::SurfaceConfiguration {
        usage,
        format: wgpu::TextureFormat::Bgra8Unorm,
        width: w.max(1),
        height: h.max(1),
        present_mode: present_mode(interval, Some(&caps)),
        alpha_mode: caps.alpha_modes.first().copied().unwrap_or(wgpu::CompositeAlphaMode::Opaque),
        view_formats: vec![],
        desired_maximum_frame_latency: 2,
    }
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

/// Reinterpret a &[u8] as a plain byte slice for write_buffer (already &[u8]; kept
/// as a named helper for symmetry with bytemuck_dst).
fn bytemuck_bytes(b: &[u8]) -> &[u8] {
    b
}

/// Shared bind group layout (uniform + texture + sampler), created once.
fn create_bind_group_layout(ctx: &RgContext) -> wgpu::BindGroupLayout {
    ctx.device().create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("raindrop d9 bgl"),
        entries: &[
            wgpu::BindGroupLayoutEntry {
                binding: 0,
                visibility: wgpu::ShaderStages::VERTEX_FRAGMENT,
                ty: wgpu::BindingType::Buffer {
                    ty: wgpu::BufferBindingType::Uniform,
                    // one bind group per (texture, filter); the per-draw uniform
                    // slot is selected with a dynamic offset
                    has_dynamic_offset: true,
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

/// Shared sampler (nearest or linear filtering), created once per mode.
fn create_sampler(ctx: &RgContext, filtering: bool) -> wgpu::Sampler {
    ctx.device().create_sampler(&wgpu::SamplerDescriptor {
        label: Some("raindrop d9 samp"),
        mag_filter: if filtering { wgpu::FilterMode::Linear } else { wgpu::FilterMode::Nearest },
        min_filter: if filtering { wgpu::FilterMode::Linear } else { wgpu::FilterMode::Nearest },
        mipmap_filter: wgpu::FilterMode::Nearest,
        address_mode_u: wgpu::AddressMode::Repeat,
        address_mode_v: wgpu::AddressMode::Repeat,
        ..Default::default()
    })
}

impl Renderer {
    /// Copy the scene (D3D9 backbuffer) into a system-memory level: the
    /// GetRenderTargetData path used by the C shim's GetDC readback.
    pub fn readback_scene(&mut self, dst: &mut crate::vtbl::SysLevel) -> Hr {
        let Some(scene) = &self.scene else { return D3DERR_NOTAVAILABLE };
        let (w, h) = (self.scene_w, self.scene_h);
        if dst.pitch < (w * 4) as usize || dst.data.len() < dst.pitch * h as usize {
            return D3DERR_INVALIDCALL;
        }
        // staging buffer with a 256-aligned bytes-per-row
        let bpr = (w * 4).div_ceil(256) * 256;
        let buf = self.ctx.device().create_buffer(&wgpu::BufferDescriptor {
            label: Some("raindrop d9 readback"),
            size: (bpr as u64) * (h as u64),
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });
        let mut enc = self.ctx.device().create_command_encoder(&wgpu::CommandEncoderDescriptor {
            label: Some("raindrop d9 readback enc"),
        });
        enc.copy_texture_to_buffer(
            scene.texture().as_image_copy(),
            wgpu::TexelCopyBufferInfo {
                buffer: &buf,
                layout: wgpu::TexelCopyBufferLayout {
                    offset: 0,
                    bytes_per_row: Some(bpr),
                    rows_per_image: None,
                },
            },
            wgpu::Extent3d { width: w, height: h, depth_or_array_layers: 1 },
        );
        self.ctx.queue().submit(Some(enc.finish()));
        let (tx, rx) = std::sync::mpsc::channel::<Result<(), wgpu::BufferAsyncError>>();
        buf.slice(..).map_async(wgpu::MapMode::Read, move |r| {
            let _ = tx.send(r);
        });
        let _ = self.ctx.device().poll(wgpu::PollType::Wait);
        if rx.try_recv().is_err() {
            return D3DERR_NOTAVAILABLE;
        }
        let mapped = buf.get_mapped_range(..);
        for y in 0..h {
            let src = &mapped[(y * bpr) as usize..(y * bpr + w * 4) as usize];
            let off = y as usize * dst.pitch;
            let out = &mut dst.data[off..off + w as usize * 4];
            out.copy_from_slice(src);
        }
        drop(mapped);
        buf.unmap();
        S_OK
    }
}
