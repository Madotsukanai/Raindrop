// raindrop-d3d9: Rust IDirect3DDevice9 emulation on wgpu (fixed-function DX9
// subset). wgpu is the only backend; there is no Backend= setting.

// COM/Win32 FFI surface: field naming follows the D3D9 headers, fn-item->usize casts
// are the deliberate vtable construction technique.
#![allow(non_snake_case, non_camel_case_types)]
#![allow(unused_variables)]
#![allow(unused_imports)]
// fn-item -> usize casts are the vtable construction technique (all slots are ABI-sized fn ptrs)
#![allow(trivial_casts)]
#![allow(function_item_references)]
#![allow(clippy::fn_to_numeric_cast)]

pub mod com;
pub mod d3d9;
pub mod device;
pub mod d9;
pub mod render;
pub mod types_gen;
pub mod vtbl;
pub mod vtbl_gen;

// COM/Win32 FFI surface: field naming follows the D3D9 headers, fn-item->usize casts
use com::*;

// Note: this crate does not export Direct3DCreate9 itself. Consumers own the
// export surface: raindrop-ddraw (五月雨 DirectDraw emulation) links it
// statically, and the standalone raindrop_d3d9.dll below exposes
// raindrop_d3d9_create9 for a thin C shim if needed.

/// ABI probe.
#[no_mangle]
pub extern "system" fn raindrop_d3d9_abi_version() -> u32 {
    1
}
/// C ABI entry used by the C d3d9.dll shim (statically linked).
/// Creates the wgpu-backed IDirect3D9 object, or NULL when wgpu is unavailable.
/// stdcall, WINAPI-compatible.
#[no_mangle]
pub extern "system" fn raindrop_d3d9_create9(sdk_version: u32) -> *mut core::ffi::c_void {
    d9::create_d3d9(sdk_version) as *mut core::ffi::c_void
}
