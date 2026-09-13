#!/bin/bash
# Link step, kept separate because the stub TU needs the same flags.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
G="$HERE/../../aquaplus_gpl"
i686-w64-mingw32-g++ -c "$HERE/shim/stubs.cpp" -o "$HERE/build/stubs.o" \
  -I"$HERE/shim/include" -I"$G/XViD/XVidDec" -I"$G/OGG/oggDec" \
  -I"$G/OGG/ogg/include" -I"$G/OGG/vorbis/include" \
  -include "$HERE/shim/th2ref_prelude.h" -m32 -w -fpermissive -funsigned-char -std=gnu++17 || exit 1
i686-w64-mingw32-g++ -m32 -static -o "$HERE/build/th2ref.exe" "$HERE"/build/*.o \
  -lddraw -ldsound -ldxguid -lstrmiids -lwinmm -lavifil32 -lmsacm32 \
  -lcomctl32 -lcomdlg32 -lole32 -luuid -lshell32 -ladvapi32 -lgdi32 -luser32 -lkernel32 || exit 1
cp "$HERE/build/th2ref.exe" "$HERE/run/"
echo "linked: $(stat -c%s "$HERE/build/th2ref.exe") bytes"
