#!/bin/sh
# Converts the demo models for the web page (ETC2 + 256 px RGBA8 fallback) into build/wasm/models.
# Usage: web/build-models.sh [convert-binary]   (default build/mac-release/convert or build/mac-debug/convert)
set -e
cd "$(dirname "$0")/.."
CONVERT=${1:-build/mac-release/convert}; [ -x "$CONVERT" ] || CONVERT=build/mac-debug/convert
M=../ppka-wannabe-2/public/model3d
OUT=build/wasm/models; mkdir -p "$OUT"
conv() { "$CONVERT" "$1" "$OUT/$2" --max-texture 512 --target web ${3:+--ktx2 "$3"}; }
conv build/assets/cache/pk-cc203.glb cc203.emod $M/ktx2/pk-cc203.glb
conv $M/nry-k1-25-wilis-badan.glb wilis.emod
conv $M/nry-jr205-kuha-badan-v2.glb krl-kuha.emod
ls -la "$OUT"
