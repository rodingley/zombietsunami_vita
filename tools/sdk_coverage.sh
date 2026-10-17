#!/bin/bash
# Reports which of the symbols the game needs already exist in your VitaSDK
# libraries (vitaGL, newlib, libm, zlib ...). Writes sdk_coverage.txt.
# usage: tools/sdk_coverage.sh        (VITASDK must be set)
NM="${NM:-arm-vita-eabi-nm}"
LIBDIR="${LIBDIR:-$VITASDK/arm-vita-eabi/lib}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-sdk_coverage.txt}"
if [ ! -d "$LIBDIR" ]; then echo "cannot find $LIBDIR - is VITASDK set?"; exit 1; fi
DEFINED="$(mktemp)"; NEEDED="$(mktemp)"
trap 'rm -f "$DEFINED" "$NEEDED"' EXIT
for f in "$LIBDIR"/*.a; do
  "$NM" -g --defined-only -P "$f" 2>/dev/null | awk 'NF>=2 && $2 ~ /^[TDBRWVuvi]$/ {print $1}'
done | sort -u > "$DEFINED"
sort -u "$HERE/needed_symbols.txt" > "$NEEDED"
{
  echo "# Symbols the game needs that your SDK libraries DEFINE ($(comm -12 "$NEEDED" "$DEFINED" | wc -l | tr -d ' '))"
  comm -12 "$NEEDED" "$DEFINED"
  echo
  echo "# Symbols the game needs that NO SDK library defines ($(comm -23 "$NEEDED" "$DEFINED" | wc -l | tr -d ' '))"
  comm -23 "$NEEDED" "$DEFINED"
} > "$OUT"
head -1 "$OUT"; grep -m1 "NO SDK" "$OUT"
echo "Wrote $OUT - attach it to the chat."
