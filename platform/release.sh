#!/usr/bin/env bash
# Publishes the version in CMakeLists.txt's project() as GitHub release vX.Y.Z of vamsikalagaturu/jackdaw: release
# builds (JD_RELEASE=1) for Linux and Android, the tag on the commit built, and the assets jackdaw-linux-x86_64 and
# jackdaw.apk. The APK never trusts the test server's CA. .github/workflows/release.yml runs it once CI passed on
# main; by hand, only on a clean main that is pushed.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
VERSION=$(sed -n 's/^project(jackdaw VERSION \([0-9.]*\).*/\1/p' "$REPO/CMakeLists.txt")
TAG=v$VERSION

[ -z "$(git -C "$REPO" status --porcelain --untracked-files=no)" ] || { echo "commit first: the release is built from HEAD" >&2; exit 1; }
gh release view "$TAG" -R vamsikalagaturu/jackdaw >/dev/null 2>&1 && { echo "$TAG exists; raise project() VERSION" >&2; exit 1; }

export JD_RELEASE=1
"$HERE/linux/build.sh" release
JD_TEST_CA= "$HERE/android/build.sh"

OUT=$(mktemp -d)
cp "${JD_LINUX_OUT:-$HOME/.cache/jackdaw/linux}/build/jackdaw" "$OUT/jackdaw-linux-$(uname -m)"
cp "${JD_ANDROID_OUT:-$HOME/.cache/jackdaw/android}/out/jackdaw.apk" "$OUT/jackdaw.apk"
gh release create "$TAG" -R vamsikalagaturu/jackdaw --target "$(git -C "$REPO" rev-parse HEAD)" --title "jackdaw $VERSION" \
    --generate-notes "$OUT/jackdaw-linux-$(uname -m)" "$OUT/jackdaw.apk"
echo "released $TAG"
