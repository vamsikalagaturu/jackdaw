#!/usr/bin/env bash
# Fetches the sources pinned in platform/versions.sh into thirdparty/ and builds raylib (GLFW, Wayland and X11),
# libetpan and libical into $JD_LINUX_OUT (default ~/.cache/jackdaw/linux). build.sh sources this file first.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
. "$REPO/platform/versions.sh"
ROOT=${JD_LINUX_OUT:-$HOME/.cache/jackdaw/linux}
JOBS=$(nproc)

for tool in cc c++ cmake ninja git; do
    command -v $tool >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done

step() { printf '\n== %s\n' "$*"; }
mkdir -p "$ROOT"

step "raylib $RAYLIB_TAG (Wayland and X11), sources"
fetch_sources "$REPO"
RAYLIB_OUT=$ROOT/raylib-install-$BUILD_TAG
if [ ! -f "$RAYLIB_OUT/lib/libraylib.a" ]; then
    cmake -S "$REPO/thirdparty/raylib" -B "$ROOT/raylib-build-$BUILD_TAG" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_EXAMPLES=OFF -DCMAKE_C_FLAGS_RELEASE="$SIZE_CFLAGS -DNDEBUG" -DGLFW_BUILD_WAYLAND=ON -DGLFW_BUILD_X11=ON \
        -DCMAKE_INSTALL_PREFIX="$RAYLIB_OUT" -DCMAKE_INSTALL_LIBDIR=lib >/dev/null
    cmake --build "$ROOT/raylib-build-$BUILD_TAG" --target install -j"$JOBS" >/dev/null
fi

step "libetpan $LIBETPAN_VERSION (static, over the system's OpenSSL and zlib)"
for pc in openssl zlib; do
    pkg-config --exists $pc || { echo "missing: $pc development files" >&2; exit 1; }
done
for tool in autoreconf libtoolize make; do
    command -v $tool >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done
LIBETPAN_OUT=$ROOT/libetpan-$LIBETPAN_VERSION-$BUILD_TAG
if [ ! -f "$LIBETPAN_OUT/lib/libetpan.a" ]; then
    # autotools build in place, so it runs in a copy and thirdparty/ stays pristine
    rm -rf "$ROOT/libetpan-src"
    cp -a "$REPO/thirdparty/libetpan-$LIBETPAN_VERSION" "$ROOT/libetpan-src"
    (cd "$ROOT/libetpan-src" && NOCONFIGURE=1 ./autogen.sh >/dev/null 2>&1 &&
        ./configure --prefix="$LIBETPAN_OUT" --libdir="$LIBETPAN_OUT/lib" --disable-shared --enable-static --with-pic \
            --with-openssl --without-gnutls --without-sasl --without-curl --without-expat --with-zlib --with-poll \
            --disable-db --disable-lockfile --enable-ipv6 CFLAGS="$SIZE_CFLAGS -DNDEBUG" >/dev/null &&
        make -j"$JOBS" >/dev/null && make install >/dev/null)
fi

step "libical $LIBICAL_VERSION (static)"
command -v perl >/dev/null || { echo "missing: perl (libical generates sources with it)" >&2; exit 1; }
LIBICAL_OUT=$ROOT/libical-$LIBICAL_VERSION-$BUILD_TAG
[ -f "$LIBICAL_OUT/lib/libical.a" ] || build_libical "$LIBICAL_OUT" "$REPO"
