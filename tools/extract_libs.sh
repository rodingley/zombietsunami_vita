#!/bin/bash
# Pulls the four 32-bit libraries out of the APKMirror .apkm bundle.
# usage: tools/extract_libs.sh /path/to/bundle.apkm [output_folder]
set -e
APKM="$1"
OUT="${2:-zombietsunami_libs}"
if [ -z "$APKM" ] || [ ! -f "$APKM" ]; then
  echo "usage: $0 /path/to/bundle.apkm [output_folder]"; exit 1
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
unzip -o -q "$APKM" split_config.armeabi_v7a.apk -d "$TMP"
mkdir -p "$OUT"
for L in libc++_shared.so libfmod.so libfmodstudio.so libcgame.so; do
  unzip -o -j -q "$TMP/split_config.armeabi_v7a.apk" "lib/armeabi-v7a/$L" -d "$OUT"
done
ls -l "$OUT"
echo "Copy this folder's four files to ux0:data/zombietsunami/lib/ on the Vita."
