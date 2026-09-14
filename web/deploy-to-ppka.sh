#!/bin/sh
# Copies the web build of the engine into the reference client (ppka-wannabe-2) for the Ayana renderer
# (src/tiga-ayana/duniaAyana.ts, `?renderer=ayana`):
#   build/wasm/ayana.{js,wasm,data} + web/ktx2.js + worker + Basis transcoder -> public/ayana/
#   build/wasm/models/*.emod (web/build-models.sh --all)                    -> public/ayana/models/   (gitignored)
#   assets/terrain/<map>/index.json (fetch_tiles)                           -> public/ayana/terrain/<map>/index.json (gitignored;
#                                                                               the browser fetches the tiles themselves from the tile servers)
#   bridge/sim-state.ts                                                      -> src/tiga-ayana/simState.ts (copy; the
#                                                                               adapter cannot import outside its repo)
# In production the models/ and terrain/ folders go to R2 (tools/unggah-r2.mjs in ppka-wannabe-2) under the same
# `ayana/` prefix; AYANA_BASE in duniaAyana.ts then points at asetUrl('ayana').
# Usage: web/deploy-to-ppka.sh [map ...]   (default: mojokerto bks)
set -e
cd "$(dirname "$0")/.."
PPKA=${PPKA:-../ppka-wannabe-2}
MAPS=${*:-mojokerto bks}
[ -f build/wasm/ayana.js ] || { echo "build first: cmake --preset wasm && cmake --build --preset wasm --target ayana"; exit 1; }
D=$PPKA/public/ayana
mkdir -p "$D/vendor" "$D/models" "$D/terrain" "$PPKA/src/tiga-ayana"
cp build/wasm/ayana.js build/wasm/ayana.wasm build/wasm/ayana.data "$D/"
cp web/ktx2.js web/ktx2-worker.js "$D/"
cp web/vendor/basis_transcoder.js web/vendor/basis_transcoder.wasm "$D/vendor/"
rsync -a --delete build/wasm/models/ "$D/models/" --include='*.emod' --exclude='ktx2/'
for m in $MAPS; do
  [ -f "assets/terrain/$m/index.json" ] || { echo "no terrain index for $m (run: build/mac-release/fetch_tiles $m)"; continue; }
  rm -rf "$D/terrain/$m"; mkdir -p "$D/terrain/$m"; cp "assets/terrain/$m/index.json" "$D/terrain/$m/"
done
{ echo "// SALINAN OTOMATIS dari ../game-engine-experiment/bridge/sim-state.ts (web/deploy-to-ppka.sh) — jangan disunting di sini."; cat bridge/sim-state.ts; } > "$PPKA/src/tiga-ayana/simState.ts"
du -sh "$D" "$D/models" "$D/terrain"
