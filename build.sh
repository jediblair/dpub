#!/bin/bash
# Builds dpub.exe from main.c (UI), engine.c (MuPDF threads) and the static
# MuPDF / mimalloc libraries in third_party (see README).
#   bash build.sh            optimized build against the LTO+PGO MuPDF libs
#   bash build.sh dev        quick build against the plain -O2 libs
set -e
cd "$(dirname "$0")"
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
mkdir -p /d/epub/src/third_party/tmp && export TMP="$(cygpath -w /d/epub/src/third_party/tmp)" TEMP="$(cygpath -w /d/epub/src/third_party/tmp)"
MU=third_party/mupdf-src
MI=third_party/mimalloc

if [ "$1" = dev ]; then
	LIBDIR=$MU/build/release-tofu-tofu_cjk-tofu_cjk_ext-tofu_cjk_lang
	OPT="-O2 -march=x86-64-v2"
else
	LIBDIR=$MU/build/pgo
	OPT="-O3 -march=x86-64-v2 -flto=auto -fno-fat-lto-objects"
fi

[ -f third_party/mimalloc.o ] || gcc -O3 -DNDEBUG -march=x86-64-v2 -I$MI/include -c $MI/src/static.c -o third_party/mimalloc.o
windres app.rc -O coff -o app_res.o
rm -f dpub.exe
gcc $OPT -Wall -Wno-unused-parameter -Wno-missing-braces \
	-I$MU/include -I$MI/include main.c engine.c app_res.o third_party/mimalloc.o -o dpub.exe \
	$LIBDIR/libmupdf.a $LIBDIR/libmupdf-third.a \
	-static -mwindows -Wl,--gc-sections -s \
	-lcomctl32 -lcomdlg32 -lshell32 -ldwmapi -lgdi32 -luser32 -ladvapi32 -lole32 -luuid -lbcrypt 2>&1 \
	| grep -v "stringop\|opj\|l_tmp\|l_data_ptr\|^ *[0-9]* |\|^ *|\|note:\|In function\|inlined from\|lto-wrapper" || true
[ -f dpub.exe ] || { echo "BUILD FAILED"; exit 1; }
ls -l dpub.exe
