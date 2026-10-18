#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Build the MuPDF static libs.  usage: third_party/mk.sh [build] [extra XCFLAGS] [extra make args]
cd "$(dirname "$0")/mupdf-src"
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
mkdir -p "$ROOT/third_party/tmp" && export TMP="$(cygpath -w "$ROOT/third_party/tmp")" TEMP="$TMP"
mingw32-make -j28 libs OS=MINGW build=${1:-release} XCFLAGS="-msse4.1 -DFZ_ALLOC_THREADSAFE $2" $3 tofu=yes tofu_cjk=yes tofu_cjk_ext=yes tofu_cjk_lang=yes \
	xps=no svg=no mujs=no extract=no brotli=no barcode=no tesseract=no commercial=no HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no > ../mk.log 2>&1
st=$?
grep -E "error" ../mk.log | grep -v "error: pcls" | head -20
exit $st
