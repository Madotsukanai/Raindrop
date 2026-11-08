#!/bin/sh
# src/Smd.c のコントローラー対応部分 (JoyBridge) を切り出す。
# 使い方: cd ~/Projects/RefRain && sh extract_joy.sh
cd "$(dirname "$0")" || exit 1
OUT=.smd_extract
mkdir -p "$OUT"
F=src/Smd.c
J=$(grep -n -m1 'Modern controller support' "$F" | cut -d: -f1)
[ -n "$J" ] && sed -n "${J},$((J + 1400))p" "$F" > "$OUT/joy.txt"
echo "J=$J"
wc -l "$OUT/joy.txt"
