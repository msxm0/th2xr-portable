#!/bin/bash
# Reference build: the GPL ToHeart2 engine, cross-compiled for win32.
# The GPL tree is never modified - case-mapped includes come from shim/,
# assembly-free copies of five files from gen/, and everything MSVC supplied
# implicitly comes from shim/th2ref_prelude.h via -include.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
G="$HERE/../../aquaplus_gpl"
CXX=i686-w64-mingw32-g++
OUT="$HERE/build"
mkdir -p "$OUT"

INC="-I$HERE/shim/include \
 -I$G/ToHeart2/ScriptEngine/src -I$G/ToHeart2/my_inc2 -I$G/ToHeart2/ScriptEngine/mes \
 -I$G/XViD/XVidDec -I$G/XViD/xvidcore/src \
 -I$G/OGG/oggDec -I$G/OGG/ogg/include -I$G/OGG/vorbis/include"
# -funsigned-char is not a liberty: ToHeart2.vcproj has
#     DefaultCharIsUnsigned="TRUE"
# i.e. the original was built with MSVC /J.  Without it LZS_DecodeMemory
# sign-extends its flag byte (`flags = *src2 | 0xff00` with signed char) and
# every compressed file in the PAKs fails its size check.
FLAGS="-m32 -O1 -w -fpermissive -fno-strict-aliasing -funsigned-char -std=gnu++17 \
 -include $HERE/shim/th2ref_prelude.h -DWIN32 -D_WIN32 -DNDEBUG ${TH2REF_FLAGS:-}"

# five files come from gen/ with the MSVC asm removed; the rest straight from the GPL tree
DEASM="MM_std Draw24 DrawPrim24 Draw32 DrawPrim32 GM_Avg Winmain GM_Demo readFile Escript main"
sources=()
for f in "$G"/ToHeart2/my_inc2/*.cpp "$G"/ToHeart2/my_inc2/*.CPP "$G"/ToHeart2/ScriptEngine/src/*.cpp; do
    [ -e "$f" ] || continue
    base=$(basename "$f"); stem="${base%.*}"
    skip=0
    for d in $DEASM; do [ "$stem" = "$d" ] && skip=1; done
    [ $skip -eq 1 ] && continue
    sources+=("$f")
done
for d in $DEASM; do sources+=("$HERE/gen/$d.cpp"); done

ok=0; fail=0; failed=()
for f in "${sources[@]}"; do
    base=$(basename "$f"); stem="${base%.*}"
    if $CXX -c "$f" -o "$OUT/$stem.o" $INC $FLAGS 2> "$OUT/$stem.log"; then
        ok=$((ok+1))
    else
        fail=$((fail+1)); failed+=("$stem")
    fi
done
echo "compiled: $ok   failed: $fail"
[ $fail -gt 0 ] && printf '  %s\n' "${failed[@]}"
exit 0
