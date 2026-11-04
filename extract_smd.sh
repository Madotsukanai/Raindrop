#!/bin/sh
# Smd.c の D3D9 レイヤー部分を小さなファイルに切り出す(解析用)。
# 使い方: cd ~/Projects/RefRain && sh extract_smd.sh
cd "$(dirname "$0")" || exit 1
OUT=.smd_extract
mkdir -p "$OUT"

# 全体の関数・型の索引 (行番号つき)
grep -n -E '^(static|typedef|#define|struct|__declspec|BOOL|HRESULT|D7FN)' Smd.c > "$OUT/index.txt"

# D3D7 -> D3D9 エミュレーション本体: 最初の D7_ から IDirect3DDevice7 の vtable 関数群の手前まで
S=$(grep -n -m1 'D7_' Smd.c | cut -d: -f1)
E=$(grep -n -m1 'EV_QueryInterface' Smd.c | cut -d: -f1)
[ -n "$S" ] && [ -n "$E" ] && S=$((S > 6 ? S - 6 : 1)) && sed -n "${S},$((E - 1))p" Smd.c > "$OUT/d3d9_core.txt"

# ゲーム側の描画まわりのパッチ (バッチ化・テクスチャ設定のスキップなど)
P=$(grep -n -m1 'PatchGamePerf(void)' Smd.c | cut -d: -f1)
if [ -n "$P" ]; then
  A=$((P > 400 ? P - 400 : 1))
  sed -n "${A},$((P + 120))p" Smd.c > "$OUT/gameperf.txt"
fi

# 描画バッチ化 (BatchDrawPrimitive 等) とフレーム待ち (FrameTimerHook) の部分
B1=$(grep -n -m1 'static const int g_BatchDraw' Smd.c | cut -d: -f1)
B2=$(grep -n -m1 'static void PatchKeepRunningInactive' Smd.c | cut -d: -f1)
[ -n "$B1" ] && [ -n "$B2" ] && sed -n "${B1},$((B2 + 45))p" Smd.c > "$OUT/batch_framewait.txt"

echo "S=$S E=$E P=$P B1=$B1 B2=$B2"
wc -l "$OUT"/*.txt
