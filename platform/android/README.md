# jackdaw for Android (arm64-v8a and x86_64, Android 11+)

## Requirements

- Android SDK at `~/Android/Sdk` (`ANDROID_HOME` overrides): build-tools 36.1.0, platform android-36.1,
  NDK 28.2.13676358 (`ANDROID_NDK` overrides).
- JDK (`javac`, `keytool`), CMake, Ninja, curl, git, zip, perl and make (OpenSSL), autotools (libetpan).

## Build

```sh
platform/android/build.sh          # -> ~/.cache/jackdaw/android/out/jackdaw.apk
platform/android/build.sh check    # only compiles libjackdaw.so
JD_ABIS=x86_64 platform/android/build.sh   # the emulator's ABI alone, faster
```

`deps.sh` builds OpenSSL, libcurl, GNU libiconv (bionic's iconv converts no 8-bit charset) and libetpan for each
ABI once, under `~/.cache/jackdaw/android`
(`JD_ANDROID_OUT` overrides), which also keeps the debug signing key. The APK trusts the test server's CA
(`~/.cache/jackdaw/imap-test/ca.crt`, from `tests/imap/start.sh`) besides the system's roots when that file exists;
`JD_TEST_CA= platform/android/build.sh` builds without it.

A build without `JD_RELEASE=1` is the separate app `io.github.vamsikalagaturu.jackdaw.dev` ("jackdaw dev"), so it
installs beside a release and never touches its data.

## Run in the emulator

```sh
emulator -avd jackdaw-test -no-audio -no-boot-anim -gpu host &   # AVD: android-35 x86_64, pixel_6, hw.keyboard=yes
tests/imap/start.sh
for p in 9993 9143 9465 9587 14190 9080; do adb reverse tcp:$p tcp:$p; done   # the test server as localhost
adb install -r ~/.cache/jackdaw/android/out/jackdaw.apk
adb shell am start -n io.github.vamsikalagaturu.jackdaw.dev/io.github.vamsikalagaturu.jackdaw.JackdawActivity
```

Test account: `jackdaw@localhost`, user `jackdaw`, password `jackdaw-test`, IMAP `localhost` 9993 TLS, SMTP
`localhost` 9465 TLS.

## Files

| File | What |
| --- | --- |
| `android_main.c` | `platform.h` on Android: environment (`HOME`, `XDG_*` in the app's files, the system's CA folder), taps, long press (right click), swipes (wheel, flings, sideways for the tab bar), Back as Esc, hardware and soft keyboard text, insets, and the Java calls. |
| `java/.../JackdawActivity.java` | `NativeActivity`: soft keyboard text, clipboard, insets, opening links and files, picking files to attach, the account password (`accounts.sealed`, AES-GCM under an Android Keystore key). |
| `java/.../OpenProvider.java` | Serves an attachment read-only to the app it is opened in (`content://<package>.files/<path>`). |
| `res/mipmap-*/ic_launcher.png` | Lucide `bird` in #88C0D0 on #2E3440. |

On a phone there are no vim modes: compose always types, Back closes what is open, and the full sidebar is a
drawer over the list. Not yet on Android: notifications and printing.
