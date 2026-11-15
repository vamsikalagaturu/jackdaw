# Third-party sources the build scripts use, and the flags they are compiled with. Sourced by platform/*/deps.sh.

SIZE_CFLAGS="-O2 -ffunction-sections -fdata-sections"
# build folders are named after the settings, so a change rebuilds everything with them
BUILD_TAG=o2

RAYLIB_TAG=6.2
RAYLIB_URL=https://github.com/raysan5/raylib.git

# SVG icons (Lucide) rasterised at run time; no releases upstream: a commit of master
NANOSVG_COMMIT=239e102ec2c691f2902e20ace2ed36ee4a35cfe6
NANOSVG_URL=https://raw.githubusercontent.com/memononen/nanosvg/$NANOSVG_COMMIT
NANOSVG_H_SHA256=e34fd5d084be106cea972d19ce5d27fd96d17ba89f8d06bdceee058420c8b2b0
NANOSVGRAST_H_SHA256=79a9c5f4db19debf9f3a648a1589e96d92854f245a5cb4f3d823f263785234d8
NANOSVG_LICENSE_SHA256=e82bc3dd03400aecabe12201219ba14750dbc4b36faab58663a7a6068548d372

# HTML mail layout (BSD-3-Clause, bundles gumbo, Apache-2.0), C++17
LITEHTML_VERSION=0.10
LITEHTML_URL=https://github.com/litehtml/litehtml/archive/refs/tags/v$LITEHTML_VERSION.tar.gz
LITEHTML_SHA256=7700eced92847d34ad9846b138cf195a9c974b519be70de58797880ae9da649e

# IMAP, SMTP and MIME (BSD-style licence), static, over OpenSSL
LIBETPAN_VERSION=1.10.1
LIBETPAN_URL=https://github.com/dinhvh/libetpan/archive/refs/tags/$LIBETPAN_VERSION.tar.gz
LIBETPAN_SHA256=87bacdc62661a2a7aa5fe9f1f28d2f7c7a53256633ac5129903916c59f80c4c2

# the amalgamation (public domain), compiled by CMakeLists.txt; sqlite.org publishes SHA3-256
# (b6ced585...d9c4, checked), the SHA-256 here was computed from that verified file
SQLITE_VERSION=3540100
SQLITE_URL=https://sqlite.org/2026/sqlite-autoconf-$SQLITE_VERSION.tar.gz
SQLITE_SHA256=926ef8d3eab93fbf6e06011901273eec076b25594a4e1cdadd50c987f4f6d64e

# download $1 to $2 once and check it against SHA-256 $3
fetch() {
    [ -f "$2" ] || { curl -fL -o "$2.part" "$1" && mv "$2.part" "$2"; }
    local sum
    sum=$( (sha256sum "$2" 2>/dev/null || shasum -a 256 "$2") | cut -d' ' -f1)
    [ "$sum" = "$3" ] || { echo "checksum mismatch for $2: $sum" >&2; return 1; }
}

# tarball $1 (SHA-256 $2) unpacked as thirdparty/$3 whatever its top folder is called ($4 = repo root)
fetch_unpacked() {
    [ -d "$4/thirdparty/$3" ] && return 0
    fetch "$1" "$4/thirdparty/$3.tar.gz" "$2"
    mkdir -p "$4/thirdparty/$3.part"
    tar -xzf "$4/thirdparty/$3.tar.gz" -C "$4/thirdparty/$3.part" --strip-components=1
    mv "$4/thirdparty/$3.part" "$4/thirdparty/$3"
}

fetch_sources() {
    mkdir -p "$1/thirdparty"
    [ -d "$1/thirdparty/raylib" ] || git clone --depth 1 --branch "$RAYLIB_TAG" "$RAYLIB_URL" "$1/thirdparty/raylib"
    mkdir -p "$1/thirdparty/nanosvg"
    fetch "$NANOSVG_URL/src/nanosvg.h" "$1/thirdparty/nanosvg/nanosvg.h" "$NANOSVG_H_SHA256"
    fetch "$NANOSVG_URL/src/nanosvgrast.h" "$1/thirdparty/nanosvg/nanosvgrast.h" "$NANOSVGRAST_H_SHA256"
    fetch "$NANOSVG_URL/LICENSE.txt" "$1/thirdparty/nanosvg/LICENSE.txt" "$NANOSVG_LICENSE_SHA256"
    fetch_unpacked "$LIBETPAN_URL" "$LIBETPAN_SHA256" "libetpan-$LIBETPAN_VERSION" "$1"
    fetch_unpacked "$LITEHTML_URL" "$LITEHTML_SHA256" "litehtml-$LITEHTML_VERSION" "$1"
    fetch_unpacked "$SQLITE_URL" "$SQLITE_SHA256" "sqlite-$SQLITE_VERSION" "$1"
}
