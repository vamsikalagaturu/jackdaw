#!/usr/bin/env bash
# Publishes the version in CMakeLists.txt's project() as GitHub release vX.Y.Z of vamsikalagaturu/jackdaw: release
# builds (JD_RELEASE=1) for Linux and Android, the tag on the commit built, the assets jackdaw-linux-x86_64 and
# jackdaw.apk, and the version's section of CHANGELOG.md as its notes. The APK never trusts the test server's CA. .github/workflows/release.yml runs it once CI passed on
# main; by hand, only on a clean main that is pushed.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
VERSION=$(sed -n 's/^project(jackdaw VERSION \([0-9.]*\).*/\1/p' "$REPO/CMakeLists.txt")
TAG=v$VERSION

[ -z "$(git -C "$REPO" status --porcelain --untracked-files=no)" ] || { echo "commit first: the release is built from HEAD" >&2; exit 1; }
gh release view "$TAG" -R vamsikalagaturu/jackdaw >/dev/null 2>&1 && { echo "$TAG exists; raise project() VERSION" >&2; exit 1; }
# the notes are the version's section of CHANGELOG.md, from its heading to the next version's
NOTES=$(awk -v v="$VERSION" 'index($0, "## [" v "]") == 1 { on = 1; next } on && /^## \[/ { exit } on' "$REPO/CHANGELOG.md")
[ -n "$(printf %s "$NOTES" | tr -d '[:space:]')" ] || { echo "CHANGELOG.md has no section for $VERSION" >&2; exit 1; }

export JD_RELEASE=1
# the build reads its plain version from this tag on HEAD (CMakeLists.txt); gh pushes it with the release
git -C "$REPO" tag -f "$TAG" HEAD
"$HERE/linux/build.sh" release
JD_TEST_CA= "$HERE/android/build.sh"

OUT=$(mktemp -d)
cp "${JD_LINUX_OUT:-$HOME/.cache/jackdaw/linux}/build/jackdaw" "$OUT/jackdaw-linux-$(uname -m)"
cp "${JD_ANDROID_OUT:-$HOME/.cache/jackdaw/android}/out/jackdaw.apk" "$OUT/jackdaw.apk"
printf '%s\n' "$NOTES" > "$OUT/notes.md"
gh release create "$TAG" -R vamsikalagaturu/jackdaw --target "$(git -C "$REPO" rev-parse HEAD)" --title "jackdaw $VERSION" \
    --notes-file "$OUT/notes.md" "$OUT/jackdaw-linux-$(uname -m)" "$OUT/jackdaw.apk"
echo "released $TAG"
