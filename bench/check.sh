#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Rebuild verify/bench against the current MuPDF libs, check output against
# reference.txt and print timings.  usage: bench/check.sh [libdir] [extra cflags]
set -e
cd "$(dirname "$0")"
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
mkdir -p "$ROOT/third_party/tmp" && export TMP="$(cygpath -w "$ROOT/third_party/tmp")" TEMP="$TMP"
L=${1:-../third_party/mupdf-src/build/release-tofu-tofu_cjk-tofu_cjk_ext-tofu_cjk_lang}
X=${2:-}
I=../third_party/mupdf-src/include
gcc -O2 -msse4.1 $X -I$I verify.c $L/libmupdf.a $L/libmupdf-third.a -o verify.exe
gcc -O2 -msse4.1 $X -I$I bench.c sampler.c $L/libmupdf.a $L/libmupdf-third.a -lwinmm -lpsapi -o bench.exe
S=../third_party/samples
(cd $S && ../../bench/verify.exe war.mobi bible.mobi war.epub bible.epub alice.epub alice.mobi pdfref.pdf 2>/dev/null) > verify.txt
if diff -q reference.txt verify.txt >/dev/null; then echo "VERIFY: identical to reference"; else echo "VERIFY: MISMATCH"; diff reference.txt verify.txt; fi
(cd $S && ../../bench/bench.exe war.mobi bible.mobi war.epub bible.epub alice.epub pdfref.pdf 2>/dev/null)
