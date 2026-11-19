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

# iCalendar (RFC 5545): parsing, recurrence and time zones of calendar events (MPL-2.0 / LGPL-2.1), static; no
# .asc upstream, the SHA-256 is of the release download
LIBICAL_VERSION=4.0.8
LIBICAL_URL=https://github.com/libical/libical/releases/download/v$LIBICAL_VERSION/libical-$LIBICAL_VERSION.tar.gz
LIBICAL_SHA256=cd486bb3ece8256c34f1a236e174992c66ecb46b592a65f3f55d24d49eb66a5d

# JSON of Google's OAuth token answers (MIT); two files at a release tag, compiled by CMakeLists.txt
CJSON_VERSION=1.7.19
CJSON_URL=https://raw.githubusercontent.com/DaveGamble/cJSON/v$CJSON_VERSION
CJSON_C_SHA256=298581a04a36c0165da4b0aade235c23088cb2faa58651d720ea2f3706ed0b0d
CJSON_H_SHA256=25b0145150d500498e4d209cec69c18c42cf818bffcc54690be3b895a2a16dee
CJSON_LICENSE_SHA256=a36dda207c36db5818729c54e7ad4e8b0c6fba847491ba64f372c1a2037b6d5c

# Android only (Linux links the system's): the XML of CalDAV's WebDAV answers (MIT); the SHA-256 is of the tarball
# whose signature (Sebastian Pipping, key CB8DE70A90CFBF6C3BF5CC5696262ACFFBD3AEC6) was checked
EXPAT_VERSION=2.10.0
EXPAT_URL=https://github.com/libexpat/libexpat/releases/download/R_${EXPAT_VERSION//./_}/expat-$EXPAT_VERSION.tar.gz
EXPAT_SHA256=5466a341f9c692ef7a958a6881311be597f8e1ce345984cf78414f54793dea4c

# Android only (Linux links the system's): OpenSSL for libetpan and libcurl (3.5 is the LTS line, to April 2030),
# and libcurl for remote images in mail
OPENSSL_VERSION=3.5.9
OPENSSL_URL=https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL_VERSION/openssl-$OPENSSL_VERSION.tar.gz
OPENSSL_SHA256=603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a
CURL_VERSION=8.23.0
CURL_URL=https://curl.se/download/curl-$CURL_VERSION.tar.xz
CURL_SHA256=8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
# Android only: bionic's iconv opens no 8-bit charset (ISO-8859-*, Windows-125x), which mail headers and bodies use;
# the SHA-256 is of the tarball whose GNU signature (Bruno Haible, key B6301D9E1BBEAC08) was checked
LIBICONV_VERSION=1.19.1
LIBICONV_URL=https://ftp.gnu.org/gnu/libiconv/libiconv-$LIBICONV_VERSION.tar.gz
LIBICONV_SHA256=9a06844846d664d7122161700c10a64480d7c7bcead3091925a785b38c91edab

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

# tarball $1 (SHA-256 $2, .tar.xz) unpacked as thirdparty/$3 ($4 = repo root)
fetch_unpacked_xz() {
    [ -d "$4/thirdparty/$3" ] && return 0
    fetch "$1" "$4/thirdparty/$3.tar.xz" "$2"
    mkdir -p "$4/thirdparty/$3.part"
    tar -xJf "$4/thirdparty/$3.tar.xz" -C "$4/thirdparty/$3.part" --strip-components=1
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
    fetch_unpacked "$LIBICAL_URL" "$LIBICAL_SHA256" "libical-$LIBICAL_VERSION" "$1"
    mkdir -p "$1/thirdparty/cjson-$CJSON_VERSION"
    fetch "$CJSON_URL/cJSON.c" "$1/thirdparty/cjson-$CJSON_VERSION/cJSON.c" "$CJSON_C_SHA256"
    fetch "$CJSON_URL/cJSON.h" "$1/thirdparty/cjson-$CJSON_VERSION/cJSON.h" "$CJSON_H_SHA256"
    fetch "$CJSON_URL/LICENSE" "$1/thirdparty/cjson-$CJSON_VERSION/LICENSE" "$CJSON_LICENSE_SHA256"
}

# libical $LIBICAL_VERSION, static, into $1 with the CMake arguments that follow (a toolchain for Android): the
# C library alone, with the system's zoneinfo (VTIMEZONEs in the calendar data cover what that lacks)
build_libical() {
    local out=$1 repo=$2
    shift 2
    cmake -Wno-author -S "$repo/thirdparty/libical-$LIBICAL_VERSION" -B "$out/libical.build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="$SIZE_CFLAGS -DNDEBUG" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_INSTALL_PREFIX="$out" -DCMAKE_INSTALL_LIBDIR=lib -DLIBICAL_STATIC=ON -DLIBICAL_CXX_BINDINGS=OFF \
        -DLIBICAL_JAVA_BINDINGS=OFF -DLIBICAL_GLIB=OFF -DLIBICAL_GOBJECT_INTROSPECTION=OFF -DLIBICAL_BUILD_DOCS=OFF \
        -DLIBICAL_BUILD_TESTING=OFF -DLIBICAL_BUILD_EXAMPLES=OFF -DCMAKE_DISABLE_FIND_PACKAGE_ICU=ON "$@" >/dev/null
    cmake --build "$out/libical.build" --target install -j"$JOBS" >/dev/null
}
