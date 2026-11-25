// Win32 bits the emu needs: ini backend name, DIB sections for GetDC, logs.

use crate::surf::SurfObj;

#[link(name = "user32")]
extern "system" {
    fn CreateCompatibleDC(hdc: isize) -> isize;
    fn DeleteDC(hdc: isize) -> i32;
    fn SelectObject(hdc: isize, obj: isize) -> isize;
    fn GetObjectA(obj: isize, n: i32, out: *mut u8) -> i32;
}

#[link(name = "gdi32")]
extern "system" {
    fn CreateDIBSection(hdc: isize, bmi: *const BmpInfo, usage: u32, bits: *mut *mut u8, section: isize, offset: u32) -> isize;
    fn DeleteObject(obj: isize) -> i32;
}

#[repr(C)]
struct BmpInfoHeader {
    size: u32,
    width: i32,
    height: i32,
    planes: u16,
    bit_count: u16,
    compression: u32,
    size_image: u32,
    x_pels: i32,
    y_pels: i32,
    clr_used: u32,
    clr_important: u32,
}

#[repr(C)]
struct BmpInfo {
    hdr: BmpInfoHeader,
    masks: [u32; 3],
}

const BI_BITFIELDS: u32 = 3;
const DIB_RGB_COLORS: u32 = 0;

/// The backend reported by GetDeviceIdentifier: wgpu is the only backend.
pub fn backend_name() -> &'static str {
    "wgpu"
}

/// Create (once) a top-down DIB section mirroring the surface, and a memory DC
/// with it selected (C ES_GetDC semantics: CreateCompatibleDC(NULL)).
pub fn dib_get(s: *mut SurfObj) -> Option<isize> {
    unsafe {
        let s = &mut *s;
        if s.dib == 0 {
            let bi = BmpInfo {
                hdr: BmpInfoHeader {
                    size: 40,
                    width: s.w,
                    height: -s.h,
                    planes: 1,
                    bit_count: s.bpp as u16,
                    compression: BI_BITFIELDS,
                    size_image: 0,
                    x_pels: 0,
                    y_pels: 0,
                    clr_used: 0,
                    clr_important: 0,
                },
                masks: [s.fmt.m[0], s.fmt.m[1], s.fmt.m[2]],
            };
            let mut bits: *mut u8 = core::ptr::null_mut();
            let dib = CreateDIBSection(0, &bi, DIB_RGB_COLORS, &mut bits, 0, 0);
            if dib == 0 || bits.is_null() {
                return None;
            }
            core::ptr::copy_nonoverlapping(s.data.as_ptr(), bits, s.data.len());
            s.dib = dib;
        } else {
            dib_sync_from_surface(s);
        }
        let dc = CreateCompatibleDC(0);
        if dc == 0 {
            return None;
        }
        SelectObject(dc, s.dib);
        Some(dc)
    }
}

/// Copy the surface's sysmem pixels into its DIB section.
pub fn dib_sync_from_surface(s: *mut SurfObj) -> bool {
    unsafe {
        let s = &mut *s;
        if s.dib == 0 {
            return false;
        }
        let mut buf = [0u8; 64];
        let n = GetObjectA(s.dib, 64, buf.as_mut_ptr());
        if n < 24 {
            return false;
        }
        // BITMAP.lpBits lives at byte offset 20
        let p = core::ptr::read_unaligned(buf.as_ptr().add(20) as *const *mut u8);
        if p.is_null() {
            return false;
        }
        core::ptr::copy_nonoverlapping(s.data.as_ptr(), p, s.data.len());
        true
    }
}

/// ES_ReleaseDC: the DC is a memory DC, delete it.
pub fn release_dc(hdc: isize) {
    unsafe {
        DeleteDC(hdc);
    }
}

/// Drop the DIB of a released surface.
pub fn dib_drop(s: *mut SurfObj) {
    unsafe {
        let s = &mut *s;
        if s.dib != 0 {
            DeleteObject(s.dib);
            s.dib = 0;
        }
    }
}

pub fn log(msg: &str) {
    eprintln!("{}", msg);
}
