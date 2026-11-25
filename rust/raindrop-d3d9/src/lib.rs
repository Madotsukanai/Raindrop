// raindrop-d3d9: Rust d3d9.dll for RefRain.
//
// Two modes (raindrop.ini [Graphics] Backend=):
//   system (default) - proxy: Direct3DCreate9 forwards to the real system d3d9.dll,
//                      exactly like the C shim. Zero behavior change.
//   wgpu             - the emulated fixed-function device in this crate.
//
// NOTE: the C d3d9.dll also patches the game binary; this crate does NOT contain
// that logic yet. Until it is ported, deploy the Rust DLL only together with a
// patched setup or for testing.

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

// Note: this crate no longer exports Direct3DCreate9 itself. The C shim
// (src/RefRain.c) owns the export surface and decides system-proxy vs wgpu
// backend via raindrop.ini, calling raindrop_d3d9_create9 (statically linked).

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
