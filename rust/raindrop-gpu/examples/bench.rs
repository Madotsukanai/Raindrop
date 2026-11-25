// Quick host-side sanity benchmark: scalar vs fearless_simd upload kernel.
// Run: cargo run --release --example bench
use raindrop_gpu::kernels::{self, Fmt16};
use std::time::Instant;

const W: usize = 640;
const H: usize = 480;
const FRAMES: usize = 60;

fn bench(label: &str, f: impl Fn(&mut [u32], &[u16])) {
    let src: Vec<u16> = (0..W * H).map(|i| (i * 2654435761) as u16).collect();
    let mut dst = vec![0u32; W * H];
    // warmup
    f(&mut dst, &src);
    let t = Instant::now();
    for _ in 0..FRAMES {
        f(&mut dst, &src);
    }
    let el = t.elapsed();
    println!(
        "{:12} {:?}/frame ({:.1} us/frame), throughput {:.1} MB/s",
        label,
        el / FRAMES as u32,
        el.as_secs_f64() * 1e6 / FRAMES as f64,
        (W * H * 2) as f64 / 1e6 / el.as_secs_f64() * FRAMES as f64,
    );
    // checksum so the compiler can't dead-code it
    println!("{:12} checksum {:08x}", label, dst.iter().fold(0u32, |a, &v| a ^ v));
}

fn main() {
    println!("565 -> ARGB8888 upload, {}x{}, {} frames", W, H, FRAMES);
    bench("scalar", |d, s| kernels::upload_conv16_scalar(d, s, Fmt16::Rgb565, None));
    bench("simd", |d, s| kernels::upload_conv16(d, s, Fmt16::Rgb565, None));
    bench("scalar+key", |d, s| {
        kernels::upload_conv16_scalar(d, s, Fmt16::Rgb565, Some((0x0000, 0xFFFF)))
    });
    bench("simd+key", |d, s| {
        kernels::upload_conv16(d, s, Fmt16::Rgb565, Some((0x0000, 0xFFFF)))
    });
}
