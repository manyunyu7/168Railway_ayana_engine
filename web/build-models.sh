#!/bin/sh
# Stages the web demo into build/wasm: geometry-only .emod files (convert --textures external, ~1 MB each), the
# reference project's KTX2 GLBs (Basis ETC1S, transcoded in the browser by web/ktx2.js) and the page's JS.
# Usage: web/build-models.sh [convert-binary]       viewer demo: 3 models + page
#        web/build-models.sh --all [map ...]         every catalog slot the maps need (default mojokerto bks) ->
#                                                    build/wasm/models/<slot>.emod (":" in an id becomes "__"), for
#                                                    the ppka-wannabe-2 adapter (web/deploy-to-ppka.sh copies them)
set -e
cd "$(dirname "$0")/.."
CONVERT=build/mac-release/convert; [ -x "$CONVERT" ] || CONVERT=build/mac-debug/convert
PPKA=${PPKA:-../ppka-wannabe-2}
M=$PPKA/public/model3d   # reference project (PPKA overrides the location)
OUT=build/wasm/models; mkdir -p "$OUT/ktx2"
if [ "$1" = "--all" ]; then
  shift; MAPS=${*:-mojokerto bks}
  LIST=$(cd "$PPKA" && npx tsx "$OLDPWD/web/slots-for-maps.mts" $MAPS)
  n=0
  echo "$LIST" | while read -r id berkas; do
    [ -n "$id" ] || continue
    src="$M/$berkas"; [ -f "$src" ] || src="build/assets/cache/$berkas"
    if [ ! -f "$src" ]; then echo "skip $id: $berkas not found"; continue; fi
    dst="$OUT/$(echo "$id" | sed 's/:/__/g').emod"
    if [ ! -f "$dst" ] || [ "$src" -nt "$dst" ]; then
      "$CONVERT" "$src" "$dst" --target web --textures external --max-texture 1024 >/dev/null 2>&1 || echo "convert failed: $id"
    fi
    n=$((n+1))
  done
  echo "$LIST" | wc -l | xargs echo "slots:"; du -sh "$OUT"
  exit 0
fi
[ -n "$1" ] && CONVERT=$1
conv() { "$CONVERT" "$1" "$OUT/$2" --target web --textures external; cp "$M/ktx2/$3" "$OUT/ktx2/$3"; }
conv build/assets/cache/pk-cc203.glb cc203.emod pk-cc203.glb
conv $M/nry-k1-25-wilis-badan.glb wilis.emod nry-k1-25-wilis-badan.glb
conv $M/nry-jr205-kuha-badan-v2.glb krl-kuha.emod nry-jr205-kuha-badan-v2.glb
# page + loader (index.html is also copied by CMake at configure time)
mkdir -p build/wasm/vendor
cp web/index.html web/ktx2.js web/ktx2-worker.js build/wasm/
cp web/vendor/basis_transcoder.js web/vendor/basis_transcoder.wasm build/wasm/vendor/
ls -la "$OUT" "$OUT/ktx2"
