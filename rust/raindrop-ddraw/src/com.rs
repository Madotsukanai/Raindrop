// Minimal COM plumbing for the DirectDraw7/Direct3D7 emulation objects.
// Objects are #[repr(C)] structs whose first two fields are { vt, refs }, so
// plain { void** vt; LONG ref; } C code (the mod's batch vtable patcher) sees
// exactly the layout it patches today.

use core::ffi::c_void;

pub type Hr = i32;

pub const DD_OK: Hr = 0;
pub const DD_FALSE: Hr = 1;
pub const E_NOINTERFACE: Hr = -2_147_467_262i32; // 0x80004002
pub const E_POINTER: Hr = -2_147_482_621i32; // 0x80004003
pub const DDERR_GENERIC: Hr = -2_147_467_259i32; // 0x80004005
pub const DDERR_UNSUPPORTED: Hr = -2_147_467_263i32; // 0x80004001
pub const DDERR_INVALIDPARAMS: Hr = -2_147_024_809i32; // 0x80070057 (E_INVALIDARG)
pub const DDERR_OUTOFMEMORY: Hr = -2_147_024_882i32; // 0x8007000E
pub const DDERR_INVALIDPIXELFORMAT: Hr = 0x88760091u32 as i32;
pub const DDERR_NOCOLORKEY: Hr = 0x887600D7u32 as i32;
pub const DDERR_SURFACELOST: Hr = 0x887601C2u32 as i32;
pub const DDERR_NOTFOUND: Hr = 0x887600FFu32 as i32;
pub const DDERR_CANTCREATEDC: Hr = 0x88760249u32 as i32;
pub const DDERR_NOPALETTEATTACHED: Hr = 0x8876023Cu32 as i32;
pub const DDERR_NOCLIPPERATTACHED: Hr = 0x88760238u32 as i32;
pub const DDERR_ALREADYINITIALIZED: Hr = 0x88760005u32 as i32;
pub const DDENUMRET_CANCEL: Hr = 0;
pub const DDENUMRET_OK: Hr = 1;

/// COM object header shared by every DDraw-side object.
#[repr(C)]
pub struct Obj {
    pub vt: *mut *mut c_void,
    pub refs: i32,
}

/// Leaked, MUTABLE slot table: the C mod patches slots in place (batch hooks).
pub fn leak_slots(entries: Vec<*mut c_void>) -> *mut *mut c_void {
    Box::into_raw(entries.into_boxed_slice()) as *mut *mut c_void
}

/// Convenience: 1-based extra refcount that never frees (singleton objects).
pub fn singleton_addref(obj: *mut Obj) -> u32 {
    unsafe {
        (*obj).refs += 1;
        (*obj).refs.max(1) as u32
    }
}
pub fn singleton_release(obj: *mut Obj) -> u32 {
    unsafe {
        if (*obj).refs > 1 {
            (*obj).refs -= 1;
        }
        (*obj).refs.max(1) as u32
    }
}
