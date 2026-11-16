#!/usr/bin/env bash
# Builds libjackdaw.so for arm64-v8a and x86_64 (the emulator) and packages one debug-signed APK, without Gradle.
# `build.sh check` only compiles the libraries. JD_TEST_CA (default the test server's, from tests/imap/start.sh)
# is trusted besides the system's roots when the file exists; `JD_TEST_CA= build.sh` leaves it out.
# Output: $JD_ANDROID_OUT/out/jackdaw.apk (default ~/.cache/jackdaw/android, which also keeps the debug signing
# key: a new key would stop the APK installing over an older one).
set -euo pipefail

MODE=${1:-}
case "$MODE" in
'' | check) ;;
*) echo "usage: $0 [check]" >&2; exit 1 ;;
esac

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$HERE/deps.sh"

TEST_CA=${JD_TEST_CA-$HOME/.cache/jackdaw/imap-test/ca.crt}
[ -n "$TEST_CA" ] && [ ! -f "$TEST_CA" ] && TEST_CA=
BT=$SDK/build-tools/36.1.0
ANDROID_JAR=$SDK/platforms/android-36.1/android.jar
# 35+ enforces edge-to-edge, which raylib's fixed framebuffer cannot follow
TARGET_API=34
if [ -z "$MODE" ]; then
    for tool in "$BT/aapt2" "$BT/d8" "$BT/zipalign" "$BT/apksigner" "$ANDROID_JAR"; do
        [ -e "$tool" ] || { echo "missing: $tool" >&2; exit 1; }
    done
    for tool in javac keytool zip; do
        command -v $tool >/dev/null || { echo "missing: $tool" >&2; exit 1; }
    done
fi

for ABI in $ABIS; do
    step "libjackdaw.so ($ABI)${TEST_CA:+, trusting $TEST_CA}"
    cmake --fresh -S "$HERE" -B "$ROOT/build-$ABI" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=$ABI -DANDROID_PLATFORM=android-$MIN_API -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release \
        -DJD_ANDROID_DEPS="$(deps_prefix $ABI)" -DJD_TEST_CA="$TEST_CA" \
        -DCMAKE_C_FLAGS_RELEASE="$SIZE_CFLAGS -DNDEBUG" -DCMAKE_CXX_FLAGS_RELEASE="$SIZE_CFLAGS -DNDEBUG" >/dev/null
    cmake --build "$ROOT/build-$ABI" --target jackdaw -j"$JOBS"
done
if [ "$MODE" = check ]; then exit 0; fi
VERSION_NAME=$(cat "$ROOT/build-$(set -- $ABIS; echo $1)/version.txt")
JD_VERSION=$(sed -n 's/^project(jackdaw VERSION \([0-9.]*\).*/\1/p' "$REPO/CMakeLists.txt")

step "dex"
APK=$ROOT/apk
rm -rf "$APK"
mkdir -p "$APK/classes" "$APK/dex"
javac -nowarn --release 11 -Xlint:-options -cp "$ANDROID_JAR" -d "$APK/classes" $(find "$HERE/java" -name '*.java')
"$BT/d8" --release --min-api $MIN_API --lib "$ANDROID_JAR" --output "$APK/dex" $(find "$APK/classes" -name '*.class')

PKG=io.github.vamsikalagaturu.jackdaw
MANIFEST=$HERE/AndroidManifest.xml
# a dev build is its own app beside the release; the provider's authority must be unique on the device too
if [ "${JD_RELEASE:-}" != 1 ]; then
    MANIFEST=$APK/AndroidManifest.xml
    sed -e "s/$PKG\.files/$PKG.dev.files/" -e 's/android:label="jackdaw"/android:label="jackdaw dev"/' \
        "$HERE/AndroidManifest.xml" >"$MANIFEST"
    RENAME=(--rename-manifest-package "$PKG.dev")
else
    RENAME=()
fi

step "resources ($VERSION_NAME)"
"$BT/aapt2" compile --dir "$HERE/res" -o "$APK/res.zip"
# versionCode must grow with each release: 1.2.3 -> 10203
IFS=. read -r V_MAJOR V_MINOR V_PATCH <<<"$JD_VERSION"
"$BT/aapt2" link -o "$APK/unsigned.apk" -I "$ANDROID_JAR" --manifest "$MANIFEST" "${RENAME[@]}" -R "$APK/res.zip" \
    --min-sdk-version $MIN_API --target-sdk-version $TARGET_API --debug-mode --auto-add-overlay \
    --version-name "$VERSION_NAME" --version-code $((V_MAJOR * 10000 + V_MINOR * 100 + V_PATCH))

step "package"
cp "$APK/dex/classes.dex" "$APK/classes.dex"
for ABI in $ABIS; do
    mkdir -p "$APK/lib/$ABI"
    "$TC/llvm-strip" --strip-unneeded -o "$APK/lib/$ABI/libjackdaw.so" "$ROOT/build-$ABI/libjackdaw.so"
done
(cd "$APK" && zip -q -r unsigned.apk classes.dex lib)
"$BT/zipalign" -P 16 -f 4 "$APK/unsigned.apk" "$APK/aligned.apk"

mkdir -p "$ROOT/out"
KS=$ROOT/debug.keystore
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias androiddebugkey \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --ks-key-alias androiddebugkey \
    --out "$ROOT/out/jackdaw.apk" "$APK/aligned.apk"
"$BT/apksigner" verify "$ROOT/out/jackdaw.apk"
ls -l "$ROOT/out/jackdaw.apk"
