#!/bin/bash
# Pulls the game data (the "assets" folder, about 100 MB) out of the APKMirror .apkm bundle.
# usage: tools/extract_assets.sh /path/to/bundle.apkm [output_folder]
set -e
APKM="$1"
OUT="${2:-zombietsunami_assets}"
if [ -z "$APKM" ] || [ ! -f "$APKM" ]; then
  echo "usage: $0 /path/to/bundle.apkm [output_folder]"; exit 1
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
unzip -o -q "$APKM" base.apk -d "$TMP"
mkdir -p "$OUT"
# only the game's own data; ad-SDK files are not needed
unzip -o -q "$TMP/base.apk" 'assets/res/*' -d "$TMP/x"
cp -R "$TMP/x/assets/res" "$OUT/res"
echo "Extracted to $OUT/res ($(du -sh "$OUT" | cut -f1))."
echo "Copy the folder 'res' into ux0:data/zombietsunami/assets/ on the Vita"
echo "(so you end up with ux0:data/zombietsunami/assets/res/zombies/...)."
