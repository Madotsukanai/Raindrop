# raindrop-gpu (Rust)

RaindropのシムDLL向けRust実装。2つの柱:

1. **SIMDピクセルカーネル**(`src/kernels.rs`)— `Smd_d3d9.c` のCPU側ホットパス
   (`D7_ToArgb` 変換・カラーキー・アップロード)の fearless_simd 実装。
2. **wgpuバックエンド骨格**(`src/gpu.rs`, feature `wgpu-backend`)— D7Surfの
   テクスチャミラーを wgpu で持つ(システム d3d9 からの脱却ステップ)。

## セマンティクス

C側と完全一致させる(`Smd_d3d9.c` 参照):

- チャンネル拡張は `(v * 255) / ((1 << nb) - 1)` の**整数除算**(ビット複製ではない)。
  SIMDではマジック乗算で除算を置換し、テストで全入力範囲を検証:
  - `/15 → (c*2185) >> 15`, `/31 → (c*8457) >> 18`, `/63 → (c*16645) >> 20` (c = v*255)
- カラーキーは生ピクセル値で比較(`raw & key_cmp_mask == key`)。
  - アップロード経路: 合致ピクセルは **alpha 0** にして透過
  - (将来のブリット経路: 合合ピクセルは **書き込みスキップ** — 未実装)
- アップロード出力は ARGB8888 (wgpuでは `Bgra8Unorm` = A8R8G8B8 と同バイト列)。

## ビルド

```sh
# Linux (MinGW クロス、DLL本体と同一ツールチェーン)
cargo build --target i686-pc-windows-gnu --release [--features wgpu-backend]

# Windows
cargo build --target i686-pc-windows-msvc --release [--features wgpu-backend]
```

- `panic = "abort"`(release)— C境界からpanicが漏れないように必須。
- 生成物: `target/i686-pc-windows-gnu/release/raindrop_gpu.dll` / `libraindrop_gpu.a`

## テスト / 計測

```sh
cargo test                      # SIMD vs スカラー(オラクル) + マジック定数の全範囲検証
cargo run --release --example bench
```

ベンチ参考値:

1) ホスト(x86_64 AVX2、640×480 565→ARGB8888 アップロード):

| 経路 | フレーム当たり |
|---|---|
| scalar | 406 µs |
| simd | 302 µs |
| scalar+key | 244 µs |
| simd+key | 173 µs |

2) 実ターゲット環境(Wine 32bit i686、-O2、`tools/kbench.c`、旧C実装 vs Rustカーネル):

| 経路 | 旧C | Rust | 倍率 |
|---|---|---|---|
| 565 | 594–1053 µs | 232–383 µs | 2.1–2.8x |
| 565+key | 282–309 µs | 166–172 µs | 1.6–1.9x |
| 5551+key | 272–291 µs | 193–205 µs | 1.4x |
| 4444+key | 273–293 µs | 198–210 µs | 1.4x |
| X8R8G8B8 | 104–109 µs | 45–50 µs | 2.2–2.3x |
| A8R8G8B8+key | 162–166 µs | 51–52 µs | 3.1–3.2x |

旧Cの565は64KエントリLUT(256KB)へのランダムアクセスがキャッシュミス主導のため
SIMD算術変換への置換で大きく効く。キー付き32bppは比較+マスクの一括化で約3x。

3) DirectDraw APIレベル(`tools/ddbench.c`, 640×480 16bpp, 3試行の最小値):
Lock+fill+Unlock: Wine内蔵ddraw 207 µs vs 本シム 183 µs / Blt: 16.8 vs 16.2 µs。
システム内蔵ddraw(sysmem直ポインタ)と同等以上。ネイティブWindowsのシステム
ddraw、実GPU経由のPresentを含む比較は未計測。

## C統合

`include/raindrop_gpu.h` を参照。cdeclエクスポート、stdcallではない。
C側からの第一段の取り込み先:

- `Smd_d3d9.c` `D7_Tex` のダーティ矩形アップロードループ(405行〜)
  → `raindrop_upload_conv16` / `raindrop_upload_xrgb32`
- `D7_Blit`(将来)→ キーでスキップ型カーネルを追加予定

## wgpuバックエンドの現状と次段

実装済み: init/destroy(Vulkan+GL、DX12は32bitでは不可)、BGRA8テクスチャ作成、
`queue.write_texture` による部分アップロード、poll。

未実装(順):

1. `raw-window-handle` による HWND からの Surface/スワップチェーン
2. 固定関数(FVF/DrawPrimitiveUP)相当の WGSL パイプライン
   (SetRenderState/TSS/Sampler をパイプライン状態+定数バッファへ)
3. D3D7エミュ層でのディスパッチ組み込み(現行のD3D9実装と並行稼働、
   `raindrop.ini` の `API=` で切替)
