#!/bin/sh
# Stages the web demo into build/wasm: geometry-only .emod files (convert --textures external, ~1 MB each), the
# reference project's KTX2 GLBs (Basis ETC1S, transcoded in the browser by web/ktx2.js) and the page's JS.
# Usage: web/build-models.sh [convert-binary]   (default build/mac-release/convert or build/mac-debug/convert)
set -e
cd "$(dirname "$0")/.."
CONVERT=${1:-build/mac-release/convert}; [ -x "$CONVERT" ] || CONVERT=build/mac-debug/convert
M=${PPKA:-../ppka-wannabe-2}/public/model3d   # reference project (PPKA overrides the location)
OUT=build/wasm/models; mkdir -p "$OUT/ktx2"
conv() { "$CONVERT" "$1" "$OUT/$2" --target web --textures external; cp "$M/ktx2/$3" "$OUT/ktx2/$3"; }
conv build/assets/cache/pk-cc203.glb cc203.emod pk-cc203.glb
conv $M/nry-k1-25-wilis-badan.glb wilis.emod nry-k1-25-wilis-badan.glb
conv $M/nry-jr205-kuha-badan-v2.glb krl-kuha.emod nry-jr205-kuha-badan-v2.glb
# page + loader (index.html is also copied by CMake at configure time)
mkdir -p build/wasm/vendor
cp web/index.html web/ktx2.js web/ktx2-worker.js build/wasm/
cp web/vendor/basis_transcoder.js web/vendor/basis_transcoder.wasm build/wasm/vendor/
ls -la "$OUT" "$OUT/ktx2"
