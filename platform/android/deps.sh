#!/usr/bin/env bash
# Fetches the sources pinned in platform/versions.sh into thirdparty/, checks the Android SDK and NDK, and builds
# OpenSSL, libcurl, libiconv and libetpan, static, once per ABI. build.sh sources this file first and uses the paths it sets.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
. "$REPO/platform/versions.sh"
ROOT=${JD_ANDROID_OUT:-$HOME/.cache/jackdaw/android}
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
NDK=${ANDROID_NDK:-$SDK/ndk/28.2.13676358}
# Android 11: WindowInsets types
MIN_API=30
ABIS=${JD_ABIS:-"arm64-v8a x86_64"}
JOBS=$(nproc)

TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
[ -e "$TC/clang" ] || { echo "missing: $TC/clang" >&2; exit 1; }
for tool in cmake ninja curl git perl make autoreconf libtoolize; do
    command -v $tool >/dev/null || { echo "missing: $tool" >&2; exit 1; }
done

step() { printf '\n== %s\n' "$*"; }
mkdir -p "$ROOT"

step "sources"
fetch_sources "$REPO"
fetch_unpacked "$OPENSSL_URL" "$OPENSSL_SHA256" "openssl-$OPENSSL_VERSION" "$REPO"
fetch_unpacked_xz "$CURL_URL" "$CURL_SHA256" "curl-$CURL_VERSION" "$REPO"
fetch_unpacked "$LIBICONV_URL" "$LIBICONV_SHA256" "libiconv-$LIBICONV_VERSION" "$REPO"

triple() {
    case $1 in
    arm64-v8a) echo aarch64-linux-android ;;
    x86_64) echo x86_64-linux-android ;;
    *) echo "no target for ABI $1" >&2; exit 1 ;;
    esac
}

# OpenSSL, libcurl, libiconv and libetpan for ABI $1, under the folder this names (build.sh passes it as JD_ANDROID_DEPS)
deps_prefix() {
    echo "$ROOT/deps-$1-openssl-$OPENSSL_VERSION-curl-$CURL_VERSION-iconv-$LIBICONV_VERSION-etpan-$LIBETPAN_VERSION-$BUILD_TAG"
}

for ABI in $ABIS; do
    OUT=$(deps_prefix $ABI)
    [ -f "$OUT/lib/libssl.a" ] && [ -f "$OUT/lib/libcurl.a" ] && [ -f "$OUT/lib/libiconv.a" ] &&
        [ -f "$OUT/lib/libetpan.a" ] && continue
    TRIPLE=$(triple $ABI)
    mkdir -p "$OUT"

    if [ ! -f "$OUT/lib/libssl.a" ]; then
        step "OpenSSL $OPENSSL_VERSION ($ABI)"
        case $ABI in arm64-v8a) TARGET=android-arm64 ;; x86_64) TARGET=android-x86_64 ;; esac
        rm -rf "$OUT/openssl.build"
        mkdir -p "$OUT/openssl.build"
        # the libraries alone: no programs, modules or protocols a mail client does not use
        (cd "$OUT/openssl.build" && ANDROID_NDK_ROOT=$NDK PATH=$TC:$PATH perl "$REPO/thirdparty/openssl-$OPENSSL_VERSION/Configure" \
            $TARGET -D__ANDROID_API__=$MIN_API no-shared no-module no-tests no-apps no-docs no-engine no-quic no-comp \
            --prefix="$OUT" --libdir=lib $SIZE_CFLAGS >/dev/null &&
            ANDROID_NDK_ROOT=$NDK PATH=$TC:$PATH make -j"$JOBS" build_libs >/dev/null &&
            make install_dev >/dev/null)
    fi

    if [ ! -f "$OUT/lib/libcurl.a" ]; then
        step "libcurl $CURL_VERSION ($ABI): HTTP(S) only, for remote images"
        off=()
        for p in DICT FILE FTP GOPHER IMAP LDAP LDAPS MQTT POP3 RTSP SMB SMTP TELNET TFTP WEBSOCKETS IPFS ALTSVC HSTS \
            COOKIES DOH AWS NETRC KERBEROS_AUTH NEGOTIATE_AUTH BEARER_AUTH FORM_API MIME HTTPSIG; do
            off+=(-DCURL_DISABLE_$p=ON)
        done
        cmake -Wno-deprecated -S "$REPO/thirdparty/curl-$CURL_VERSION" -B "$OUT/curl.build" -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=$ABI \
            -DANDROID_PLATFORM=android-$MIN_API -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS_RELEASE="$SIZE_CFLAGS -DNDEBUG" \
            -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX="$OUT" \
            -DCMAKE_INSTALL_LIBDIR=lib -DOPENSSL_ROOT_DIR="$OUT" -DOPENSSL_INCLUDE_DIR="$OUT/include" \
            -DOPENSSL_CRYPTO_LIBRARY="$OUT/lib/libcrypto.a" -DOPENSSL_SSL_LIBRARY="$OUT/lib/libssl.a" \
            -DBUILD_CURL_EXE=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF \
            -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF -DCURL_USE_OPENSSL=ON \
            -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF -DCURL_USE_GSSAPI=OFF -DUSE_LIBIDN2=OFF \
            -DUSE_NGHTTP2=OFF -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DCURL_ENABLE_NTLM=OFF \
            -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none -DENABLE_THREADED_RESOLVER=ON -DCURL_USE_PKGCONFIG=OFF \
            "${off[@]}" >/dev/null
        cmake --build "$OUT/curl.build" --target install -j"$JOBS" >/dev/null
    fi

    if [ ! -f "$OUT/lib/libiconv.a" ]; then
        step "libiconv $LIBICONV_VERSION ($ABI)"
        rm -rf "$OUT/libiconv.build"
        mkdir -p "$OUT/libiconv.build"
        (cd "$OUT/libiconv.build" &&
            CC="$TC/clang --target=$TRIPLE$MIN_API" AR="$TC/llvm-ar" RANLIB="$TC/llvm-ranlib" \
                CFLAGS="$SIZE_CFLAGS -fPIC -DNDEBUG" \
                "$REPO/thirdparty/libiconv-$LIBICONV_VERSION/configure" --host=$TRIPLE --prefix="$OUT" \
                --libdir="$OUT/lib" --disable-shared --enable-static --with-pic --disable-nls >/dev/null &&
            make lib/localcharset.h >/dev/null && make -j"$JOBS" -C lib >/dev/null && make -C lib install >/dev/null &&
            install -m 644 include/iconv.h.inst "$OUT/include/iconv.h")
    fi

    # libetpan's iconv calls go to libiconv: its iconv.h, found first in $OUT/include, renames them
    if [ ! -f "$OUT/lib/libetpan.a" ]; then
        step "libetpan $LIBETPAN_VERSION ($ABI)"
        # autotools build in place, so it runs in a copy and thirdparty/ stays pristine
        SRC=$OUT/libetpan.src
        rm -rf "$SRC"
        cp -a "$REPO/thirdparty/libetpan-$LIBETPAN_VERSION" "$SRC"
        (cd "$SRC" && NOCONFIGURE=1 ./autogen.sh >/dev/null 2>&1 &&
            CC="$TC/clang --target=$TRIPLE$MIN_API" AR="$TC/llvm-ar" RANLIB="$TC/llvm-ranlib" \
                CPPFLAGS="-I$OUT/include" LDFLAGS="-L$OUT/lib" CFLAGS="$SIZE_CFLAGS -fPIC -DNDEBUG" \
                ./configure --host=$TRIPLE --prefix="$OUT" --libdir="$OUT/lib" --disable-shared --enable-static \
                --with-pic --with-openssl="$OUT" --without-gnutls --without-sasl --without-curl --without-expat \
                --with-zlib --with-poll --disable-db --disable-lockfile --enable-ipv6 >/dev/null &&
            make -j"$JOBS" >/dev/null && make install >/dev/null)
    fi
done
