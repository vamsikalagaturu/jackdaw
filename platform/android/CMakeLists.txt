# libjackdaw.so for the Android APK; configured by build.sh with the NDK toolchain file, once per ABI.
cmake_minimum_required(VERSION 3.20)
project(jackdaw_android C CXX)

set(JD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# raylib 6.2 for PLATFORM_ANDROID, as its own CMake builds it; defined first, so the shared build uses it
set(RAYLIB_SRC ${JD_ROOT}/thirdparty/raylib/src)
add_library(raylib STATIC ${RAYLIB_SRC}/rcore.c ${RAYLIB_SRC}/rshapes.c ${RAYLIB_SRC}/rtext.c ${RAYLIB_SRC}/rtextures.c
  ${RAYLIB_SRC}/raudio.c ${RAYLIB_SRC}/rmodels.c ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
set_target_properties(raylib PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
target_compile_definitions(raylib PUBLIC PLATFORM_ANDROID GRAPHICS_API_OPENGL_ES2)
target_include_directories(raylib PUBLIC ${RAYLIB_SRC} ${ANDROID_NDK}/sources/android/native_app_glue)
target_compile_options(raylib PRIVATE -w -fno-strict-aliasing)
# rcore_android.c defines __wrap_fopen (APK assets) on top of __real_fopen
target_link_options(raylib PUBLIC -Wl,--wrap=fopen)
target_link_libraries(raylib PUBLIC log android EGL GLESv2 OpenSLES m)

# the shared sources (jdcore, jdapp with platform/android/android_main.c); no programs or tests
set(JD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${JD_ROOT} shared EXCLUDE_FROM_ALL)
get_property(JD_VERSION_STRING GLOBAL PROPERTY JD_VERSION_STRING)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/version.txt "${JD_VERSION_STRING}\n")

add_library(jackdaw SHARED ${JD_ROOT}/src/app/main.c)
target_compile_options(jackdaw PRIVATE -Wall -Wextra -Wno-unused-parameter)
# -u keeps the glue's entry point, which nothing in the library references. The wraps let android_main.c feed
# what raylib's Android layer leaves empty: typed text, the wheel (from dragging fingers) and the clipboard.
target_link_options(jackdaw PRIVATE -u ANativeActivity_onCreate -Wl,--no-undefined -Wl,--gc-sections
  -Wl,--wrap=GetCharPressed -Wl,--wrap=GetMouseWheelMove -Wl,--wrap=GetMouseWheelMoveV -Wl,--wrap=SetClipboardText
  -Wl,--wrap=GetClipboardText
  -Wl,--exclude-libs,libssl.a:libcrypto.a:libcurl.a:libetpan.a:libjd_sqlite.a:liblitehtml.a:libgumbo.a)
target_link_libraries(jackdaw PRIVATE jdapp raylib android log m)
