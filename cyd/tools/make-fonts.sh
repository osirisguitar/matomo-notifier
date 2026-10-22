#!/usr/bin/env bash
# Regenerates src/fonts/*.h: TFT_eSPI's FreeSans GFX fonts, but covering
# U+0020-U+017F (Latin-1 + Latin Extended-A: åäö, é, ü, ø, ł, š, ...) instead
# of 7-bit ASCII. Same source TTFs (GNU FreeFont) and converter (Adafruit
# fontconvert, 141 dpi) that produced the bundled *7b fonts, so ASCII glyphs
# keep their metrics. Needs curl, unzip, a C compiler and freetype
# (brew install freetype pkg-config).
set -euo pipefail

FIRST=32
LAST=383
OUT="$(cd "$(dirname "$0")/.." && pwd)/src/fonts"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/dl/ff" "$WORK/build/fc"
curl -sSfL -o "$WORK/dl/freefont.zip" https://ftp.gnu.org/gnu/freefont/freefont-ttf-20120503.zip
curl -sSfL -o "$WORK/build/fc/fontconvert.c" https://raw.githubusercontent.com/adafruit/Adafruit-GFX-Library/master/fontconvert/fontconvert.c
curl -sSfL -o "$WORK/build/gfxfont.h" https://raw.githubusercontent.com/adafruit/Adafruit-GFX-Library/master/gfxfont.h
unzip -q "$WORK/dl/freefont.zip" -d "$WORK/dl/ff"
cc -O2 -o "$WORK/build/fontconvert" "$WORK/build/fc/fontconvert.c" $(pkg-config --cflags --libs freetype2)

TTF="$WORK/dl/ff/freefont-20120503"
for spec in FreeSans:6 FreeSans:9 FreeSans:12 FreeSansBold:12; do
  name=${spec%%:*}
  size=${spec##*:}
  "$WORK/build/fontconvert" "$TTF/$name.ttf" "$size" "$FIRST" "$LAST" > "$OUT/${name}${size}pt8b.h"
  echo "$OUT/${name}${size}pt8b.h"
done
