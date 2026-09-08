#!/usr/bin/env bash
set -euo pipefail

# Build the native dependencies of th2xr-portable for wasm32-emscripten.
#
# Everything installs into the active Emscripten sysroot
# ($(em-config CACHE)/sysroot), which is already on CMAKE_FIND_ROOT_PATH and
# PKG_CONFIG_LIBDIR for emcmake/emconfigure builds, so the project's
# find_package()/pkg_check_modules() calls work unchanged.
#
# Usage:
#   ci/build-wasm-third-party.sh              # build everything missing
#   ci/build-wasm-third-party.sh sdl3 ffmpeg  # build only these
#   FORCE=1 ci/build-wasm-third-party.sh sdl3 # rebuild even if installed

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/third_party/wasm-build"

if ! command -v emcc >/dev/null 2>&1; then
    echo "emcc not found. Source emsdk_env.sh first." >&2
    exit 1
fi

PREFIX="$(em-config CACHE)/sysroot"
JOBS="$(nproc)"
FORCE="${FORCE:-0}"

# The engine is linked with native wasm exception handling, which also selects
# the wasm setjmp/longjmp lowering.  Every object in the link has to agree, so
# the dependencies are compiled with the same flag (freetype's use of setjmp
# otherwise leaves an undefined emscripten_longjmp at link time).
WASM_FLAGS="-fwasm-exceptions"
CMAKE_FLAG_ARGS=(
    "-DCMAKE_C_FLAGS=${WASM_FLAGS}"
    "-DCMAKE_CXX_FLAGS=${WASM_FLAGS}"
)

SDL3_TAG="release-3.4.10"
SDL3_TTF_TAG="release-3.2.2"
FREETYPE_VERSION="2.13.3"
ZSTD_VERSION="1.5.7"
SQLITE_YEAR="2025"
SQLITE_RELEASE="sqlite-amalgamation-3500400"
FFMPEG_VERSION="7.1.1"

mkdir -p "${BUILD_DIR}"

log() { echo; echo "=== $* ==="; }

fetch_tarball() {
    local url="$1" file="$2" dir="$3"
    cd "${BUILD_DIR}"
    [ -f "${file}" ] || { echo "Downloading ${url}"; curl -fsSL -o "${file}" "${url}"; }
    if [ ! -d "${dir}" ]; then
        case "${file}" in
            *.zip) unzip -q "${file}" ;;
            *) tar -xf "${file}" ;;
        esac
    fi
}

have() { [ "${FORCE}" = "1" ] && return 1; [ -e "$1" ]; }

build_sdl3() {
    have "${PREFIX}/lib/libSDL3.a" && { echo "SDL3 already installed"; return; }
    log "SDL3 ${SDL3_TAG}"
    fetch_tarball \
        "https://github.com/libsdl-org/SDL/archive/refs/tags/${SDL3_TAG}.tar.gz" \
        "SDL-${SDL3_TAG}.tar.gz" "SDL-${SDL3_TAG}"
    emcmake cmake -S "${BUILD_DIR}/SDL-${SDL3_TAG}" -B "${BUILD_DIR}/sdl3-build" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
        "${CMAKE_FLAG_ARGS[@]}" \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
        -DSDL_INSTALL_TESTS=OFF
    cmake --build "${BUILD_DIR}/sdl3-build" -j "${JOBS}"
    cmake --install "${BUILD_DIR}/sdl3-build"
}

build_freetype() {
    have "${PREFIX}/lib/libfreetype.a" && { echo "freetype already installed"; return; }
    log "freetype ${FREETYPE_VERSION}"
    fetch_tarball \
        "https://downloads.sourceforge.net/project/freetype/freetype2/${FREETYPE_VERSION}/freetype-${FREETYPE_VERSION}.tar.gz" \
        "freetype-${FREETYPE_VERSION}.tar.gz" "freetype-${FREETYPE_VERSION}"
    emcmake cmake -S "${BUILD_DIR}/freetype-${FREETYPE_VERSION}" \
        -B "${BUILD_DIR}/freetype-build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
        "${CMAKE_FLAG_ARGS[@]}" \
        -DBUILD_SHARED_LIBS=OFF \
        -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
    cmake --build "${BUILD_DIR}/freetype-build" -j "${JOBS}"
    cmake --install "${BUILD_DIR}/freetype-build"
}

build_sdl3_ttf() {
    have "${PREFIX}/lib/libSDL3_ttf.a" && { echo "SDL3_ttf already installed"; return; }
    build_sdl3
    build_freetype
    log "SDL3_ttf ${SDL3_TTF_TAG}"
    fetch_tarball \
        "https://github.com/libsdl-org/SDL_ttf/archive/refs/tags/${SDL3_TTF_TAG}.tar.gz" \
        "SDL_ttf-${SDL3_TTF_TAG}.tar.gz" "SDL_ttf-${SDL3_TTF_TAG}"
    emcmake cmake -S "${BUILD_DIR}/SDL_ttf-${SDL3_TTF_TAG}" \
        -B "${BUILD_DIR}/sdl3-ttf-build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
        "${CMAKE_FLAG_ARGS[@]}" \
        -DBUILD_SHARED_LIBS=OFF \
        -DSDLTTF_VENDORED=OFF -DSDLTTF_HARFBUZZ=OFF -DSDLTTF_PLUTOSVG=OFF \
        -DSDLTTF_SAMPLES=OFF -DSDLTTF_INSTALL_MAN=OFF
    cmake --build "${BUILD_DIR}/sdl3-ttf-build" -j "${JOBS}"
    cmake --install "${BUILD_DIR}/sdl3-ttf-build"
}

build_zstd() {
    have "${PREFIX}/lib/libzstd.a" && { echo "zstd already installed"; return; }
    log "zstd ${ZSTD_VERSION}"
    fetch_tarball \
        "https://github.com/facebook/zstd/releases/download/v${ZSTD_VERSION}/zstd-${ZSTD_VERSION}.tar.gz" \
        "zstd-${ZSTD_VERSION}.tar.gz" "zstd-${ZSTD_VERSION}"
    emcmake cmake -S "${BUILD_DIR}/zstd-${ZSTD_VERSION}/build/cmake" \
        -B "${BUILD_DIR}/zstd-build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
        "${CMAKE_FLAG_ARGS[@]}" \
        -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON \
        -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF \
        -DZSTD_MULTITHREAD_SUPPORT=OFF
    cmake --build "${BUILD_DIR}/zstd-build" -j "${JOBS}"
    cmake --install "${BUILD_DIR}/zstd-build"
}

build_sqlite() {
    have "${PREFIX}/lib/libsqlite3.a" && { echo "sqlite3 already installed"; return; }
    log "sqlite3 ${SQLITE_RELEASE}"
    fetch_tarball \
        "https://www.sqlite.org/${SQLITE_YEAR}/${SQLITE_RELEASE}.zip" \
        "${SQLITE_RELEASE}.zip" "${SQLITE_RELEASE}"
    cd "${BUILD_DIR}/${SQLITE_RELEASE}"
    emcc -O2 ${WASM_FLAGS} -c sqlite3.c -o sqlite3.o \
        -DSQLITE_OMIT_LOAD_EXTENSION=1 \
        -DSQLITE_THREADSAFE=0 \
        -DSQLITE_DISABLE_LFS=1 \
        -DSQLITE_ENABLE_COLUMN_METADATA=1
    emar rcs libsqlite3.a sqlite3.o
    install -Dm644 libsqlite3.a "${PREFIX}/lib/libsqlite3.a"
    install -Dm644 sqlite3.h "${PREFIX}/include/sqlite3.h"
    install -Dm644 sqlite3ext.h "${PREFIX}/include/sqlite3ext.h"
    install -d "${PREFIX}/lib/pkgconfig"
    cat > "${PREFIX}/lib/pkgconfig/sqlite3.pc" <<EOF
prefix=${PREFIX}
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: SQLite
Description: SQL database engine
Version: 3.50.4
Libs: -L\${libdir} -lsqlite3
Cflags: -I\${includedir}
EOF
}

build_ffmpeg() {
    have "${PREFIX}/lib/libavcodec.a" && { echo "ffmpeg already installed"; return; }
    log "ffmpeg ${FFMPEG_VERSION}"
    fetch_tarball \
        "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz" \
        "ffmpeg-${FFMPEG_VERSION}.tar.xz" "ffmpeg-${FFMPEG_VERSION}"
    cd "${BUILD_DIR}/ffmpeg-${FFMPEG_VERSION}"
    make distclean >/dev/null 2>&1 || true
    emconfigure ./configure \
        --prefix="${PREFIX}" \
        --enable-cross-compile \
        --target-os=none \
        --arch=wasm32 \
        --cpu=generic \
        --cc=emcc --cxx=em++ --ar=emar --ranlib=emranlib --nm=llvm-nm \
        --objcc=emcc --dep-cc=emcc \
        --disable-asm --disable-x86asm --disable-inline-asm \
        --disable-stripping --disable-programs --disable-doc \
        --disable-autodetect --disable-network --disable-debug \
        --disable-pthreads --disable-w32threads --disable-os2threads \
        --disable-everything --disable-avdevice --disable-avfilter \
        --disable-postproc --disable-iconv \
        --enable-protocol=file \
        --enable-demuxer=wav,ogg,avi,asf,mov,matroska,mp3,mpegps,mpegts,image2 \
        --enable-parser=vorbis,mpegaudio,mpeg4video,h264,aac,vp8,vp9 \
        --enable-decoder=vorbis,mp3,mp3float,aac,ac3,wmav1,wmav2 \
        --enable-decoder=pcm_s16le,pcm_s16be,pcm_u8,pcm_s24le,pcm_s32le,pcm_f32le \
        --enable-decoder=adpcm_ms,adpcm_ima_wav \
        --enable-decoder=mpeg4,msmpeg4v1,msmpeg4v2,msmpeg4v3,mpeg1video,mpeg2video \
        --enable-decoder=h264,hevc \
        --enable-decoder=wmv1,wmv2,wmv3,vc1,vp8,vp9,theora,mjpeg,rawvideo \
        --enable-swscale --enable-swresample \
        --extra-cflags="-O2 ${WASM_FLAGS}" \
        --extra-cxxflags="-O2 ${WASM_FLAGS}"
    make -j"${JOBS}"
    make install
}

TARGETS=("$@")
if [ "${#TARGETS[@]}" -eq 0 ]; then
    TARGETS=(sdl3 freetype sdl3_ttf zstd sqlite ffmpeg)
fi

for target in "${TARGETS[@]}"; do
    case "${target}" in
        sdl3) build_sdl3 ;;
        freetype) build_freetype ;;
        sdl3_ttf) build_sdl3_ttf ;;
        zstd) build_zstd ;;
        sqlite) build_sqlite ;;
        ffmpeg) build_ffmpeg ;;
        *) echo "Unknown target: ${target}" >&2; exit 1 ;;
    esac
done

log "Done. Dependencies installed to ${PREFIX}"
