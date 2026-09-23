#!/bin/bash
# Link step, kept separate because the stub TU needs the same flags.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
G="$HERE/../../aquaplus_gpl"
# The hooks TU must NOT get the prelude: it is the one place that has to call
# the real timeGetTime rather than the macro that replaces it.
# The input TU needs the engine's keybord.h, so it gets the engine include
# path; it never calls timeGetTime so the prelude's redirect is harmless.
i686-w64-mingw32-g++ -c "$HERE/shim/th2ref_input.cpp" -o "$HERE/build/th2ref_input.o" \
  -I"$HERE/shim" -I"$HERE/shim/include" -I"$G/ToHeart2/my_inc2" \
  -I"$G/ToHeart2/ScriptEngine/src" -m32 -w -fpermissive -funsigned-char -std=gnu++17 || exit 1
# zlib for the frame dump.  Extracted from the Fedora mingw32-zlib-static
# package into thirdparty/ rather than installed, so the build needs nothing
# on the machine it is not carrying itself.
ZROOT="$HERE/thirdparty/usr/i686-w64-mingw32/sys-root/mingw"
i686-w64-mingw32-g++ -c "$HERE/shim/th2ref_hooks.cpp" -o "$HERE/build/th2ref_hooks.o" \
  -I"$HERE/shim" -I"$ZROOT/include" -m32 -w -funsigned-char -std=gnu++17 || exit 1
# The state TU is the one shim file that includes the engine's own headers -
# the fields have to be read at the real offsets - so it gets the engine
# include paths and the prelude, exactly like an engine translation unit.
i686-w64-mingw32-g++ -c "$HERE/shim/th2ref_state.cpp" -o "$HERE/build/th2ref_state.o" \
  -I"$HERE/shim" -I"$HERE/shim/include" -I"$G/ToHeart2/my_inc2" \
  -I"$G/ToHeart2/ScriptEngine/src" -I"$G/ToHeart2/mes" \
  -include "$HERE/shim/th2ref_prelude.h" \
  -m32 -w -fpermissive -funsigned-char -std=gnu++17 ${TH2REF_FLAGS:-} || exit 1
i686-w64-mingw32-g++ -c "$HERE/shim/stubs.cpp" -o "$HERE/build/stubs.o" \
  -I"$HERE/shim/include" -I"$G/XViD/XVidDec" -I"$G/OGG/oggDec" \
  -I"$G/OGG/ogg/include" -I"$G/OGG/vorbis/include" \
  -include "$HERE/shim/th2ref_prelude.h" -m32 -w -fpermissive -funsigned-char -std=gnu++17 || exit 1
i686-w64-mingw32-g++ -c "$HERE/shim/th2ref_rand.cpp" -o "$HERE/build/th2ref_rand.o" \
  -m32 -w -std=gnu++17 || exit 1
i686-w64-mingw32-g++ -m32 -static -Wl,--wrap=rand -o "$HERE/build/th2ref.exe" "$HERE"/build/*.o \
  -lddraw -ldsound -ldxguid -lstrmiids -lwinmm -lavifil32 -lmsacm32 \
  -lcomctl32 -lcomdlg32 -lole32 -luuid -lshell32 -ladvapi32 -lgdi32 -luser32 -lkernel32 \
  -L"$ZROOT/lib" -lz || exit 1
cp "$HERE/build/th2ref.exe" "$HERE/run/"
echo "linked: $(stat -c%s "$HERE/build/th2ref.exe") bytes"
