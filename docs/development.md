# Development

## Build (Linux)

```
platform/linux/build.sh [release|debug]
```

Output: `~/.cache/jackdaw/linux/build{,-debug}/jackdaw`; `debug` adds AddressSanitizer and the tests
(`ctest --test-dir ~/.cache/jackdaw/linux/build-debug`). Third-party sources are fetched into `thirdparty/`.
Needs autoconf/automake/libtool (libetpan), perl (libical), OpenSSL, zlib, libcurl, expat, libsystemd, fontconfig;
libsecret-1 for the keystore. libical (calendar data) is built from source, statically, like libetpan; cJSON is
compiled in.

A build without `JD_RELEASE=1` in the environment is `jackdaw-dev`: it keeps its settings, mail and keyring entries
apart (`~/.config/jackdaw-dev`, `~/.local/share/jackdaw-dev`), so it never touches an installed release.
The version reads plain `X.Y.Z` only when the checkout is exactly tag `vX.Y.Z` with no changes (what
`platform/release.sh` builds); every other build shows `X.Y.Z-dev+<commit>[.dirty]`, a `JD_RELEASE=1` one too.

Android: see [platform/android/README.md](../platform/android/README.md).

## Test server

`tests/imap/start.sh` runs Dovecot + Postfix in Docker with the mail `tests/imap/make_mail.py` generates (threads, a
mailing list, HTML newsletters with inline images, receipts, attachments, nine folders): user `jackdaw`, password
`jackdaw-test`, IMAPS 9993, STARTTLS 9143, SMTP 9465/9587, ManageSieve 14190, remote test images on http 9080. Start
jackdaw with `SSL_CERT_FILE=~/.cache/jackdaw/imap-test/ca.crt` to trust its certificate.

## Releases

Changes reach `main` through pull requests; CI (`.github/workflows/ci.yml`) builds Linux and Android. After CI passes
on `main`, `.github/workflows/release.yml` runs `platform/release.sh` when the version in `CMakeLists.txt`'s
`project()` has no release yet, publishing `jackdaw-linux-x86_64` and `jackdaw.apk` as `vX.Y.Z`. A release needs that
version raised.
