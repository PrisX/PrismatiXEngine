# vcpkg's community arm64-android triplet, plus PrismatiX choices:
# - API level 26 (Android 8.0), matching ANDROID_PLATFORM in the
#   android-arm64 CMake preset.
# - FFmpeg is LGPL, so it is built as shared libraries that ship next to
#   libmain.so and can be replaced. Everything else links statically into
#   libmain.so.
# - Release only: the Android Player is always built as Release, and a
#   debug variant would double the FFmpeg cross-build time.
# - 16 KB page-size compatible shared libraries (required by Google Play for
#   Android 15+ targets). CMake ports get the NDK option; FFmpeg's configure
#   build gets the linker flag directly.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_CMAKE_SYSTEM_NAME Android)
set(VCPKG_CMAKE_SYSTEM_VERSION 26)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-linux-android")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DANDROID_ABI=arm64-v8a -DANDROID_STL=c++_static
    -DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON)
set(VCPKG_LINKER_FLAGS "-Wl,-z,max-page-size=16384")

if(PORT MATCHES "^ffmpeg$")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
