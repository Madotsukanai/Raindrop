// COM plumbing for the emulated D3D9 interfaces.
//
// Layout is exactly what the game's CINTERFACE code sees: a pointer to a table of
// stdcall function pointers, then our private fields. Vtables are built once per
// interface tag at runtime (same approach as the C ddraw shim's static slot arrays).

use core::ffi::c_void;

pub type Hr = i32;

pub const S_OK: Hr = 0;
pub const D3D_OK: Hr = 0;
pub const E_NOINTERFACE: Hr = -2_147_467_262i32; // 0x80004002
pub const D3DERR_INVALIDCALL: Hr = (0x8876086Cu32) as i32; // MAKE_D3DHRESULT(2156)
pub const D3DERR_NOTAVAILABLE: Hr = (0x88760874u32) as i32; // MAKE_D3DHRESULT(2154)
pub const D3DERR_DEVICELOST: Hr = (0x88760868u32) as i32; // MAKE_D3DHRESULT(2152)
pub const D3DERR_DEVICENOTRESET: Hr = (0x88760869u32) as i32; // MAKE_D3DHRESULT(2153)

/// Which vtable an object carries.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Tag {
    D3D9,
    Device,
    SwapChain,
    Surface,
    VertexBuffer,
    IndexBuffer,
    VertexDeclaration,
    Texture,
}

/// Base COM object. `vt` points at slot 0 of a `*const c_void` slot table.
#[repr(C)]
pub struct ComObj {
    pub vt: *const *const c_void,
    pub refs: i32,
    pub tag: Tag,
    pub data: *mut c_void,
}

/// Logs an unimplemented method once per tag and returns `fallback`.
/// (kept tiny: no dependency on a logging crate in the game process)
pub fn warn_once(tag: Tag, what: &str, fallback: Hr) -> Hr {
    use std::sync::atomic::{AtomicU64, Ordering};
    static SEEN: AtomicU64 = AtomicU64::new(0);
    let bit = 1u64 << (tag as u64 * 8 % 64);
    let prev = SEEN.fetch_or(bit, Ordering::Relaxed);
    if prev & bit == 0 {
        eprintln!("raindrop-d3d9: unimplemented {:?} {} (returns {:#x})", tag, what, fallback);
    }
    fallback
}

pub fn stub(_tag: Tag, what: &str) -> Hr {
    warn_once(Tag::Device, what, D3DERR_INVALIDCALL)
}
