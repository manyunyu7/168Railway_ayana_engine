#!/bin/sh
# Android `.emod` builds: ETC2 textures embedded (convert --target android), gzipped on disk so the CDN
# can serve them with Content-Encoding: gzip. Cloudflare does NOT compress application/octet-stream by
# itself, so the object has to go up already compressed — see docs/TEXTURE-FORMAT.md.
#
# Two variants, because 512 px atlases are visibly soft in the kabin/samping cameras:
#   build/android/models/        --max-texture 512  -> 1024 px atlases   (the default, `penuh`)
#   build/android/models-hemat/  --max-texture 256  ->  512 px atlases   (low-memory devices, `hemat`)
# The converter treats --max-texture as a budget and allows 2x N, hence 512 -> 1024.
# The plugin picks the directory with AyanaModelVariant (flutter/ayana/lib/src/tile_urls.dart).
#
# Usage: web/build-models-android.sh --all [map ...]     every slot the maps need (default mojokerto bks)
#        web/build-models-android.sh <id> [id ...]       just these catalog ids (smoke test)
# Nothing is uploaded: `ls` the output and see the note this script prints at the end.
set -e
cd "$(dirname "$0")/.."
CONVERT=build/mac-release/convert; [ -x "$CONVERT" ] || CONVERT=build/mac-debug/convert
PPKA=${PPKA:-../ppka-wannabe-2}
M=$PPKA/public/model3d
OUT=build/android/models
OUT_HEMAT=build/android/models-hemat
mkdir -p "$OUT" "$OUT_HEMAT"

# id -> source glb, from the catalog (same lookup as web/build-models.sh)
list_all() {
  MAPS=${*:-mojokerto bks}
  (cd "$PPKA" && npx tsx "$OLDPWD/web/slots-for-maps.mts" $MAPS)
}

# $1 = catalog id, $2 = source file name
one() {
  id=$1; berkas=$2
  src="$M/$berkas"; [ -f "$src" ] || src="build/assets/cache/$berkas"
  if [ ! -f "$src" ]; then echo "skip $id: $berkas not found"; return 0; fi
  name=$(echo "$id" | sed 's/:/__/g').emod
  for pair in "$OUT:512" "$OUT_HEMAT:256"; do
    dir=${pair%:*}; cap=${pair#*:}
    dst="$dir/$name"
    [ -f "$dst" ] && [ "$dst" -nt "$src" ] && continue
    tmp="$dst.tmp"
    if "$CONVERT" "$src" "$tmp" --target android --textures embedded --max-texture "$cap" >/dev/null 2>&1; then
      gzip -9 -n -c "$tmp" > "$dst"      # stored compressed; the object keeps the .emod name
      rm -f "$tmp"
    else
      rm -f "$tmp"; echo "convert failed: $id ($cap)"
    fi
  done
}

if [ "$1" = "--all" ]; then
  shift
  list_all "$@" | while read -r id berkas; do [ -n "$id" ] && one "$id" "$berkas"; done
else
  [ $# -gt 0 ] || { echo "usage: $0 --all [map ...] | <catalog-id> [...]"; exit 2; }
  LIST=$(list_all mojokerto bks)   # the same id -> file mapping, filtered to the ids asked for
  for id in "$@"; do
    berkas=$(echo "$LIST" | awk -v i="$id" '$1 == i { print $2; exit }')
    [ -n "$berkas" ] || { echo "skip $id: not a slot of mojokerto/bks"; continue; }
    one "$id" "$berkas"
  done
fi

du -sh "$OUT" "$OUT_HEMAT"
cat <<'NOTE'

Upload (Henry): these files are ALREADY gzipped and must keep the `.emod` name.
  aws s3 sync build/android/models       s3://<bucket>/pk/ayana/models-android \
      --content-type application/octet-stream --content-encoding gzip --cache-control 'public,max-age=31536000,immutable'
  aws s3 sync build/android/models-hemat s3://<bucket>/pk/ayana/models-android-hemat  (same flags)
Or add two entries to ppka-wannabe-2 tools/unggah-r2.mjs next to the `ayana` one, with
Content-Encoding: gzip — without that header the phone gets gzip bytes labelled as a model
(the Dart loader inflates them anyway, but every other client would not).
NOTE
