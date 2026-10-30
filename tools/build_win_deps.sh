#!/bin/sh
# Builds the libraries the desktop app needs, from their official source
# releases, as static libraries:
#
#   third_party/win64/   for Windows (MinGW-w64 cross build)
#   third_party/linux/   SDL3 only, for the Linux development build, so both
#                        builds use the same SDL. The rest come from the
#                        distribution's -dev packages on Linux.
#
# Usage: tools/build_win_deps.sh [win64|linux|all]   (default: all)
# Needs: mingw-w64, cmake, ninja-build, meson, curl, sha256sum.
# Downloads are checked against the SHA-256 sums below.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TP="$ROOT/third_party"
SRC="$TP/src"
WIN="$TP/win64"
LNX="$TP/linux"
JOBS=$(nproc 2>/dev/null || echo 4)
HOST=x86_64-w64-mingw32

SDL_VER=3.4.18
CURL_VER=8.23.0
MPG123_VER=1.33.7
FREETYPE_VER=2.15.0
HARFBUZZ_VER=15.0.0
FRIBIDI_VER=1.0.18

# name|url|sha256
SOURCES="
SDL3-$SDL_VER.tar.gz|https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL3-$SDL_VER.tar.gz|9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
curl-$CURL_VER.tar.xz|https://curl.se/download/curl-$CURL_VER.tar.xz|8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
mpg123-$MPG123_VER.tar.bz2|https://www.mpg123.de/download/mpg123-$MPG123_VER.tar.bz2|31d0e35a4ca567ec9b5ebda6c3062bb4435d6d3eacd6ef0d95cadd7854dc03ee
freetype-$FREETYPE_VER.tar.xz|https://download.savannah.gnu.org/releases/freetype/freetype-$FREETYPE_VER.tar.xz|93cd47eb719c325107f2d3cf087d06299e944c3b48a40b55537302304dea1dd5
harfbuzz-$HARFBUZZ_VER.tar.xz|https://github.com/harfbuzz/harfbuzz/releases/download/$HARFBUZZ_VER/harfbuzz-$HARFBUZZ_VER.tar.xz|968536e9abd782f07f7954044c0c4020199fcdd39b41d221ec42d2a3be4a44df
fribidi-$FRIBIDI_VER.tar.xz|https://github.com/fribidi/fribidi/releases/download/v$FRIBIDI_VER/fribidi-$FRIBIDI_VER.tar.xz|1de5e8073d035388b71d0b2d452b1da1f8caeea8a7702997b401fe1b6250727f
"

fetch_all() {
    mkdir -p "$SRC"
    echo "$SOURCES" | while IFS='|' read -r name url sum; do
        [ -n "$name" ] || continue
        if [ ! -f "$SRC/$name" ]; then
            echo "== downloading $name"
            curl -fL --retry 3 -o "$SRC/$name.part" "$url"
            mv "$SRC/$name.part" "$SRC/$name"
        fi
        got=$(sha256sum "$SRC/$name" | cut -d' ' -f1)
        if [ "$got" != "$sum" ]; then
            echo "SHA-256 mismatch for $name: got $got, expected $sum" >&2
            exit 1
        fi
        dir="$SRC/${name%.tar.*}"
        [ -d "$dir" ] || tar -xf "$SRC/$name" -C "$SRC"
    done
}

cmake_build() { # name srcdir prefix toolchain-or-empty args...
    name=$1 src=$2 prefix=$3 tc=$4
    shift 4
    bdir="$TP/build/$(basename "$prefix")/$name"
    echo "== $name -> $prefix"
    set -- -S "$src" -B "$bdir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@"
    [ -n "$tc" ] && set -- "$@" -DCMAKE_TOOLCHAIN_FILE="$tc"
    cmake "$@" >"$TP/build/$name-$(basename "$prefix")-configure.log"
    cmake --build "$bdir" -j "$JOBS" >/dev/null
    cmake --install "$bdir" >/dev/null
}

meson_cross_file() {
    f="$TP/build/mingw-w64.meson"
    cat >"$f" <<EOF
[binaries]
c = '$HOST-gcc-posix'
cpp = '$HOST-g++-posix'
ar = '$HOST-ar'
strip = '$HOST-strip'
windres = '$HOST-windres'
pkg-config = 'pkg-config'

[properties]
pkg_config_libdir = '$WIN/lib/pkgconfig'

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
    echo "$f"
}

meson_build() { # name srcdir args...
    name=$1 src=$2
    shift 2
    bdir="$TP/build/win64/$name"
    echo "== $name -> $WIN"
    rm -rf "$bdir"
    PKG_CONFIG_LIBDIR="$WIN/lib/pkgconfig" meson setup "$bdir" "$src" \
        --cross-file "$(meson_cross_file)" --prefix "$WIN" --libdir lib \
        --buildtype release -Ddefault_library=static "$@" \
        >"$TP/build/$name-win64-configure.log"
    meson compile -C "$bdir" -j "$JOBS" >/dev/null
    meson install -C "$bdir" >/dev/null
}

build_win64() {
    tc="$ROOT/cmake/mingw-w64.cmake"
    cmake_build SDL3 "$SRC/SDL3-$SDL_VER" "$WIN" "$tc" \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF
    # Schannel: Windows' own TLS and certificate store, so no CA bundle to ship.
    cmake_build curl "$SRC/curl-$CURL_VER" "$WIN" "$tc" \
        -DCURL_USE_SCHANNEL=ON -DCURL_USE_OPENSSL=OFF -DCURL_USE_LIBPSL=OFF \
        -DUSE_LIBIDN2=OFF -DUSE_WIN32_IDN=ON -DUSE_NGHTTP2=OFF -DCURL_ZLIB=OFF \
        -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DCURL_USE_LIBSSH2=OFF \
        -DCURL_DISABLE_LDAP=ON -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
        -DBUILD_EXAMPLES=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF \
        -DENABLE_CURL_MANUAL=OFF
    cmake_build mpg123 "$SRC/mpg123-$MPG123_VER/ports/cmake" "$WIN" "$tc" \
        -DBUILD_PROGRAMS=OFF -DBUILD_LIBOUT123=OFF
    cmake_build freetype "$SRC/freetype-$FREETYPE_VER" "$WIN" "$tc" \
        -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
    meson_build harfbuzz "$SRC/harfbuzz-$HARFBUZZ_VER" \
        -Dfreetype=enabled -Dglib=disabled -Dgobject=disabled -Dcairo=disabled \
        -Dicu=disabled -Dchafa=disabled -Dtests=disabled -Ddocs=disabled \
        -Dutilities=disabled -Dintrospection=disabled
    meson_build fribidi "$SRC/fribidi-$FRIBIDI_VER" \
        -Ddocs=false -Dbin=false -Dtests=false
}

build_linux() {
    # XTest is only for SDL's own input-synthesis tests.
    cmake_build SDL3 "$SRC/SDL3-$SDL_VER" "$LNX" "" \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF \
        -DSDL_X11_XTEST=OFF
}

what=${1:-all}
mkdir -p "$TP/build"
fetch_all
case $what in
win64) build_win64 ;;
linux) build_linux ;;
all) build_linux; build_win64 ;;
*) echo "usage: $0 [win64|linux|all]" >&2; exit 2 ;;
esac
echo "== done"
