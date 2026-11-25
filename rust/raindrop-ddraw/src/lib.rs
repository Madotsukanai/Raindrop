// raindrop-ddraw: Rust replacement for Samidare's DirectDraw7/Direct3D7
// emulation layer (src/Smd/Smd_d3d9.c), running on the raindrop-d3d9 wgpu
// device. The C shim keeps the export surface + mod hooks and calls in here.

#![allow(non_snake_case, non_camel_case_types, unused_variables, unused_imports, trivial_casts, clippy::fn_to_numeric_cast)]

mod com;
mod fmt;
mod state;
mod surf;
mod vtables;
mod win;

use com::*;

/// ABI probe.
#[no_mangle]
pub extern "system" fn raindrop_ddraw_abi_version() -> u32 {
    1
}

/// C ABI entry used by the C ddraw.dll shim (statically linked).
/// interval: D3DPRESENT_INTERVAL_* chosen by the C vsync config.
/// coop_hook/win_hook/flush_hook: optional C callbacks (stdcall) for the mod
/// behavior that stays in C (hotkey attach, borderless setup, batch flush).
/// Returns DD_OK with the IDirectDraw7 object, or an error when wgpu is
/// unavailable so the caller can fall back.
#[no_mangle]
pub extern "system" fn raindrop_ddraw_create(
    out: *mut *mut core::ffi::c_void,
    interval: u32,
    coop_hook: Option<extern "system" fn(isize)>,
    win_hook: Option<extern "system" fn(isize)>,
    flush_hook: Option<extern "system" fn()>,
) -> i32 {
    if out.is_null() {
        return com::E_POINTER;
    }
    unsafe {
        *out = core::ptr::null_mut();
        match state::create(interval, coop_hook, win_hook, flush_hook) {
            Some(obj) => {
                *out = obj as *mut core::ffi::c_void;
                com::DD_OK
            }
            None => com::DDERR_GENERIC,
        }
    }
}
