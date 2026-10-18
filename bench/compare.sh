#!/bin/bash
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Build bench against a given MuPDF lib dir with given flags and print min-of-3 timings.
# usage: bench/compare.sh libdir "cflags" [label]
cd "$(dirname "$0")"
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
mkdir -p "$ROOT/third_party/tmp" && export TMP="$(cygpath -w "$ROOT/third_party/tmp")" TEMP="$TMP"
L=$1
gcc -O2 -msse4.1 $2 -I../third_party/mupdf-src/include -I../third_party/mimalloc/include -DUSE_MIMALLOC bench.c sampler.c ../third_party/mimalloc.o \
	$L/libmupdf.a $L/libmupdf-third.a -lwinmm -lpsapi -lbcrypt -o bench-cmp.exe || exit 1
echo "== ${3:-$L}"
(cd ../third_party/samples && BENCH_RUNS=3 ../../bench/bench-cmp.exe war.mobi bible.mobi war.epub bible.epub alice.epub pdfref.pdf 2>/dev/null)
