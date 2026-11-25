# raindrop-d3d9 (Rust d3d9 エミュレーション)

`IDirect3DDevice9` 固定関数パスのwgpu実装。`raindrop-gpu` クレート
(wgpu + fearless_simd カーネル)を利用する。

- RefRain 本体の `d3d9.dll` は C のみのプロキシ shim に戻したため、
  本クレートは RefRain にはリンクされない。
- 現在の利用者: `raindrop-ddraw`(五月雨 DirectDraw エミュレーション、
  静的リンク)。
- バックエンド設定は廃止済み。wgpu が唯一のバックエンド。

## wgpu エミュレーションの現状

mingwの `d3d9.h` からCOM vtableレイアウトを生成(`vtbl_gen.rs`)し、
DX9相当のCOM ABIをRustで実装している。実装済み:

- `Direct3DCreate9` → `IDirect3D9`(アダプタ列挙/CheckDevice*/GetDeviceCaps/CreateDevice)
- `IDirect3DDevice9`: BeginScene/EndScene/Clear/Present、Reset、
  CreateTexture/RenderTarget/OffscreenPlainSurface/DepthStencil(ダミー)/
  VertexBuffer/IndexBuffer、LockRect/Lock、SetTexture/SetFVF、
  RenderState/TextureStageState/SamplerStateの保存+解釈(αブレンド、αテスト)、
  `DrawPrimitive` / `DrawIndexedPrimitive` / `DrawPrimitiveUP` / `DrawIndexedPrimitiveUP`
  (XYZRHW + DIFFUSE/SPECULAR/TEX1 の固定関数パスのみ)
- テクスチャ: A8R8G8B8/X8R8G8B8/565系をsysmem管理、ダーティ時に
  `raindrop-gpu` の fearless_simd 変換カーネル経由でBgra8Unormへアップロード
  (565→8888変換はSIMDカーネルを再利用)
- Present: シーンテクスチャ(backbuffer相当)をスワップチェーンへコピーして提示。
  HWNDから raw-window-handle でSurfaceを生成。DX12は32bitプロセスでは不可のため
  バックエンドは Vulkan(+GL)

未実装(ログを出して拒否): シェーダ、パレット、ステートブロック、クエリ、
レンダーターゲットへの実際の描画(バックバッファのみ)、GetDC など。

## 検証

Wine上の実機テスト済み(2026-11):

```
Direct3DCreate9 -> COM object
GetAdapterCount / CreateDevice / BeginScene / Clear / EndScene / Present -> すべて D3D_OK
```

COM vtableレイアウトは実ゲームのCINTERFACE呼び出しと同じ経路(Cテストから
`IDirect3D9_GetAdapterCount` マクロ経由)で検証している。

## ビルド

```sh
# Linux (MinGW クロス)
cargo build --target i686-pc-windows-gnu --release --features wgpu-backend
# またはリポジトリルートで
make rust-d3d9     # -> build/rust/d3d9.dll
```

Windows(MSVC)は NMakefile の `rust-d3d9` 相当を `cargo build --target
i686-pc-windows-msvc --release --features wgpu-backend` で実施。

## 既知の未移植点(重要)

C版 `d3d9.dll` はゲームバイナリのパッチ(プラクティスメニュー等)を含む。
本クレートにはそのロジックが**無い**。フル置き換えには `src/RefRain.c` の
アドレスパッチ部分のRust移植が残課題。それまで Rust 版はテスト/実験用で、
配布は C 版を推奨。
