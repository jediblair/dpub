#!/bin/bash
# Profile-guided, link-time-optimized build of the MuPDF libs into build/pgo:
#   1. instrumented build   2. training run on the sample books   3. optimized rebuild
# usage: third_party/pgo.sh
set -e
cd "$(dirname "$0")"
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
mkdir -p /d/epub/src/third_party/tmp && export TMP="$(cygpath -w /d/epub/src/third_party/tmp)" TEMP="$(cygpath -w /d/epub/src/third_party/tmp)"
PGO="$(cygpath -m "$PWD")/pgo-data"
OPT="-O3 -march=x86-64-v2 -flto=auto -fno-fat-lto-objects"
MAKEARGS="OUT=build/pgo AR=gcc-ar RANLIB=gcc-ranlib"
I="-Imupdf-src/include -Imimalloc/include -DUSE_MIMALLOC"
L="mupdf-src/build/pgo/libmupdf.a mupdf-src/build/pgo/libmupdf-third.a mimalloc.o -lbcrypt"

rm -rf pgo-data mupdf-src/build/pgo
[ -f mimalloc.o ] || gcc -O3 -DNDEBUG -march=x86-64-v2 -Imimalloc/include -c mimalloc/src/static.c -o mimalloc.o

echo "== instrumented build"
bash mk.sh release "$OPT -fprofile-generate=$PGO -fprofile-update=atomic" "$MAKEARGS"
gcc $OPT -fprofile-generate=$PGO $I ../bench/bench.c ../bench/sampler.c $L -lwinmm -lpsapi -o tmp/bench-train.exe
gcc $OPT -fprofile-generate=$PGO $I ../bench/verify.c $L -o tmp/verify-train.exe

echo "== training"
(cd samples && ../tmp/bench-train.exe war.mobi bible.mobi war.epub bible.epub alice.epub alice.mobi pdfref.pdf sample.pdf >/dev/null 2>&1 \
	&& ../tmp/verify-train.exe war.mobi war.epub alice.epub pdfref.pdf >/dev/null 2>&1)

echo "== optimized build"
shopt -s globstar; rm -f mupdf-src/build/pgo/**/*.o
rm -f mupdf-src/build/pgo/*.a
bash mk.sh release "$OPT -fprofile-use=$PGO -fprofile-partial-training -Wno-missing-profile" "$MAKEARGS"
ls -l mupdf-src/build/pgo/*.a
