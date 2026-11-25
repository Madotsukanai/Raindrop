// Pixel format handling for DDraw surfaces: mask layout + ARGB conversion,
// ported from the C D7 emu's D7_Fmt / D7_ToArgb / D7_FromArgb.

pub const DDPF_ALPHAPIXELS: u32 = 1;
pub const DDPF_RGB: u32 = 64;
pub const DDPF_ZBUFFER: u32 = 1024;

#[derive(Clone, Copy, Default)]
pub struct Fmt {
    pub m: [u32; 4],
    pub sh: [i32; 4],
    pub nb: [i32; 4],
}

static mut EXP_TABLE: Option<Box<[u8; 9 * 256]>> = None;

fn exp_table() -> &'static [u8; 9 * 256] {
    unsafe {
        let t = &mut *core::ptr::addr_of_mut!(EXP_TABLE);
        t.get_or_insert_with(|| {
            let mut b = Box::new([0u8; 9 * 256]);
            for nb in 1..=8usize {
                for v in 0..(1usize << nb) {
                    b[nb * 256 + v] = ((v * 255) / ((1 << nb) - 1)) as u8;
                }
            }
            b
        })
    }
}

impl Fmt {
    pub fn from_pf(rgb_bit_count: u32, r: u32, g: u32, b: u32, a: u32) -> Fmt {
        let mut f = Fmt::default();
        f.m = [r, g, b, a];
        for i in 0..4 {
            if f.m[i] != 0 {
                f.sh[i] = f.m[i].trailing_zeros() as i32;
                f.nb[i] = (f.m[i] >> f.sh[i]).count_ones() as i32;
            }
        }
        let _ = rgb_bit_count;
        f
    }

    pub fn argb_to_raw(&self, argb: u32) -> u32 {
        let c = [(argb >> 16) & 255, (argb >> 8) & 255, argb & 255, argb >> 24];
        let mut raw = 0u32;
        for i in 0..4 {
            if self.nb[i] > 0 {
                let v = (c[i] * ((1u32 << self.nb[i]) - 1) + 127) / 255;
                raw |= (v << self.sh[i]) & self.m[i];
            }
        }
        raw
    }

    pub fn raw_to_argb(&self, raw: u32) -> u32 {
        let tab = exp_table();
        let mut c = [0u32; 4];
        for i in 0..4 {
            if self.nb[i] > 0 {
                let v = (raw & self.m[i]) >> self.sh[i];
                c[i] = if self.nb[i] <= 8 {
                    tab[self.nb[i] as usize * 256 + (v & 255) as usize] as u32
                } else {
                    v * 255 / ((1u32 << self.nb[i]) - 1)
                };
            } else {
                c[i] = if i == 3 { 255 } else { 0 };
            }
        }
        (c[3] << 24) | (c[0] << 16) | (c[1] << 8) | c[2]
    }

    pub fn same(&self, o: &Fmt) -> bool {
        self.m == o.m
    }
}

/// raindrop-gpu kernel code for the known 16bpp layouts, or None for generic.
pub fn fmt_code(f: &Fmt) -> Option<raindrop_gpu::kernels::Fmt16> {
    use raindrop_gpu::kernels::Fmt16;
    if f.m == [0x0000F800, 0x000007E0, 0x0000001F, 0] {
        Some(Fmt16::Rgb565)
    } else if f.m == [0x00007C00, 0x000003E0, 0x0000001F, 0] {
        Some(Fmt16::Rgb555)
    } else if f.m == [0x00007C00, 0x000003E0, 0x0000001F, 0x00008000] {
        Some(Fmt16::Rgba5551)
    } else if f.m == [0x00000F00, 0x000000F0, 0x0000000F, 0x0000F000] {
        Some(Fmt16::Rgba4444)
    } else {
        None
    }
}
