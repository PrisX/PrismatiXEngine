# Top-level CMake project for PrismatiXEngine
cmake_minimum_required (VERSION 3.21)

if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  # A default, not a requirement: CI passes -DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded
  # (/Z7) so sccache can cache MSVC compiles, which it cannot with a shared
  # PDB (/Zi). Local and release builds leave it unset and keep the PDBs.
  if (NOT DEFINED CMAKE_MSVC_DEBUG_INFORMATION_FORMAT)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<CONFIG:Debug>,EditAndContinue,$<$<CONFIG:RelWithDebInfo>:ProgramDatabase>>")
  endif()
endif()

project ("PrismatiXEngine" LANGUAGES C CXX)

# CMake can misdecode Traditional Chinese cl.exe /showIncludes output while
# configuring Ninja. A garbled prefix silently records zero header deps, so
# changing a public SDK header can leave old and new C++ class layouts linked
# into the same binary. Override only the unambiguous zh-TW-only MSVC install;
# other locales retain CMake's detected prefix.
if(MSVC AND CMAKE_GENERATOR MATCHES "^Ninja" AND
   IS_ABSOLUTE "${CMAKE_CXX_COMPILER}")
    get_filename_component(_PRISMATIX_CL_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB _PRISMATIX_CLUI_FILES "${_PRISMATIX_CL_DIR}/*/clui.dll")
    list(LENGTH _PRISMATIX_CLUI_FILES _PRISMATIX_CLUI_COUNT)
    if(_PRISMATIX_CLUI_COUNT EQUAL 1)
        get_filename_component(_PRISMATIX_CLUI_DIR "${_PRISMATIX_CLUI_FILES}" DIRECTORY)
        get_filename_component(_PRISMATIX_CLUI_LOCALE "${_PRISMATIX_CLUI_DIR}" NAME)
        if(_PRISMATIX_CLUI_LOCALE STREQUAL "1028")
            set(CMAKE_CL_SHOWINCLUDES_PREFIX "注意: 包含檔案: ")
            message(STATUS "Using the zh-TW MSVC /showIncludes prefix for Ninja header dependencies")
        endif()
    endif()
endif()

if(APPLE)
    enable_language(OBJCXX)
endif()

include(CTest)
option(PRISMATIX_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(PRISMATIX_WARNINGS_AS_ERRORS "Treat compiler warnings as release-blocking errors" OFF)
option(PRISMATIX_BUILD_FUZZERS "Build libFuzzer parser targets" OFF)
option(PRISMATIX_PERFORMANCE_TESTS "Enable scheduled large-scene performance scenarios" OFF)
option(PRISMATIX_STRESS_TESTS "Enable long-running performance and 50,000-asset stress tests" OFF)
option(PRISMATIX_HEAVY_STRESS_TESTS "Enable commercial-scale 100k-dialogue acceptance tests" OFF)
option(PRISMATIX_BUILD_WASM_PREVIEW "Build the Studio-only Emscripten preview runtime" OFF)
option(PRISMATIX_BUILD_WASM_PLAYER "Build the shipping Emscripten Web Player" OFF)

if((PRISMATIX_BUILD_WASM_PREVIEW OR PRISMATIX_BUILD_WASM_PLAYER) AND NOT EMSCRIPTEN)
    message(FATAL_ERROR
        "WASM Player and Preview require the Emscripten toolchain (configure with emcmake).")
endif()
if(PRISMATIX_BUILD_WASM_PREVIEW AND PRISMATIX_BUILD_WASM_PLAYER)
    message(FATAL_ERROR "WASM Player and Preview use separate platform compositions; configure them in separate build directories.")
endif()

function(prismatix_configure_target target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(PRISMATIX_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(PRISMATIX_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
        # Clang diagnoses intentionally partial C++20 designated initializers
        # (Clang 18 as -Wmissing-field-initializers; Clang 19+, Apple Clang 17+
        # and Emscripten also as -Wmissing-designated-field-initializers, a
        # warning option older Clangs reject as unknown). GCC and MSVC accept
        # the pattern silently. Keep the diagnostics visible without making
        # this safe default-initialization pattern release-blocking; every
        # other warning remains an error under PRISMATIX_WARNINGS_AS_ERRORS.
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE
                -Wno-error=missing-field-initializers)
            if((CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND
                    CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 17) OR
               (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
                    CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19))
                target_compile_options(${target} PRIVATE
                    -Wno-error=missing-designated-field-initializers)
            endif()
        endif()
    endif()
    if(PRISMATIX_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

if(APPLE)
    set(_PRISMATIX_HOMEBREW_PREFIXES)

    if(DEFINED ENV{HOMEBREW_PREFIX} AND EXISTS "$ENV{HOMEBREW_PREFIX}")
        list(APPEND _PRISMATIX_HOMEBREW_PREFIXES "$ENV{HOMEBREW_PREFIX}")
    endif()

    find_program(_PRISMATIX_BREW_EXECUTABLE brew)
    if(_PRISMATIX_BREW_EXECUTABLE)
        execute_process(
            COMMAND "${_PRISMATIX_BREW_EXECUTABLE}" --prefix
            OUTPUT_VARIABLE _PRISMATIX_BREW_PREFIX
            RESULT_VARIABLE _PRISMATIX_BREW_PREFIX_RESULT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(_PRISMATIX_BREW_PREFIX_RESULT EQUAL 0 AND EXISTS "${_PRISMATIX_BREW_PREFIX}")
            list(APPEND _PRISMATIX_HOMEBREW_PREFIXES "${_PRISMATIX_BREW_PREFIX}")
        endif()
    endif()

    foreach(_PRISMATIX_DEFAULT_PREFIX IN ITEMS "/opt/homebrew" "/usr/local")
        if(EXISTS "${_PRISMATIX_DEFAULT_PREFIX}")
            list(APPEND _PRISMATIX_HOMEBREW_PREFIXES "${_PRISMATIX_DEFAULT_PREFIX}")
        endif()
    endforeach()

    if(_PRISMATIX_HOMEBREW_PREFIXES)
        list(REMOVE_DUPLICATES _PRISMATIX_HOMEBREW_PREFIXES)
        foreach(_PRISMATIX_HOMEBREW_PREFIX IN LISTS _PRISMATIX_HOMEBREW_PREFIXES)
            list(APPEND CMAKE_PREFIX_PATH
                "${_PRISMATIX_HOMEBREW_PREFIX}"
            )
        endforeach()
    endif()
endif()

add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

# The NDK r27 libc++ (LLVM 18) and Apple's libc++ still ship std::jthread and
# std::stop_token as experimental. -fexperimental-library enables them and
# links libc++experimental.a (the NDK's, or the Xcode toolchain's); both are
# header-level facilities used only inside our own binaries, so the unstable
# ABI never crosses a library boundary.
if(ANDROID OR APPLE)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>")
    add_link_options(-fexperimental-library)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Dependencies (source-built for Emscripten; vcpkg/Homebrew for Native)
include(FetchContent)
if(EMSCRIPTEN)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrismatiXWasmDependencies.cmake")
else()
    find_package(SDL3 CONFIG REQUIRED)
    find_package(SDL3_image CONFIG REQUIRED)
    find_package(SDL3_ttf CONFIG REQUIRED)
    if(ANDROID AND TARGET harfbuzz::harfbuzz)
        # vcpkg's harfbuzz.pc lists -lc++, which the NDK resolves to
        # libc++_shared.so. The Player links libc++ statically
        # (ANDROID_STL=c++_static); a second, shared C++ runtime would also
        # have to ship in the APK. Drop it from SDL_ttf's harfbuzz target.
        get_target_property(_harfbuzz_libraries harfbuzz::harfbuzz INTERFACE_LINK_LIBRARIES)
        if(_harfbuzz_libraries)
            list(REMOVE_ITEM _harfbuzz_libraries c++ c++_shared stdc++)
            set_target_properties(harfbuzz::harfbuzz PROPERTIES
                INTERFACE_LINK_LIBRARIES "${_harfbuzz_libraries}")
        endif()
    endif()
    find_package(SDL3_mixer CONFIG REQUIRED)
    if(NOT ANDROID)
        # The offline shader compiler library is host-only. Android builds
        # still run the host shadercross executable for built-in shaders.
        find_package(SDL3_shadercross CONFIG REQUIRED)
        # Encoders for distribution.transcode (Packager only). SDL_image and
        # SDL_mixer already depend on these; without them the Packager reports
        # that transcoding is unavailable and packs the originals.
        find_package(WebP CONFIG QUIET)
        find_package(Ogg CONFIG QUIET)
        find_package(Vorbis CONFIG QUIET)
        find_package(Opus CONFIG QUIET)
    endif()
    find_package(nlohmann_json CONFIG REQUIRED)
    find_package(spdlog CONFIG REQUIRED)
    find_package(utf8proc CONFIG REQUIRED)
    find_package(libunibreak CONFIG REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FRIBIDI REQUIRED IMPORTED_TARGET fribidi)
    if(UNIX AND NOT APPLE AND NOT ANDROID)
        pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
    endif()
endif()
if(NOT EMSCRIPTEN)
    find_package(MbedTLS CONFIG REQUIRED)
    find_package(zstd CONFIG REQUIRED)
    find_package(FFMPEG REQUIRED)
endif()

if(NOT EMSCRIPTEN)
    add_library(prismatix_mbedcrypto INTERFACE)
    if(TARGET MbedTLS::mbedcrypto)
        target_link_libraries(prismatix_mbedcrypto INTERFACE MbedTLS::mbedcrypto)
    elseif(TARGET MbedTLS::tfpsacrypto)
        target_link_libraries(prismatix_mbedcrypto INTERFACE MbedTLS::tfpsacrypto)
    elseif(TARGET MbedTLS::mbedtls)
        target_link_libraries(prismatix_mbedcrypto INTERFACE MbedTLS::mbedtls)
    else()
        message(FATAL_ERROR "Could not find a usable MbedTLS crypto target.")
    endif()
endif()

# Embedded JavaScript runtime. Pin the exact QuickJS-NG revision exercised by
# native and Emscripten builds, and exclude its command-line tools from the
# default build: PrismatiX embeds only the C library and never ships Node.
set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_CLI_STATIC OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    quickjs_ng
    GIT_REPOSITORY https://github.com/quickjs-ng/quickjs.git
    GIT_TAG        433941b99fb3c5e7f98b7ebd78727972bcf467ee
    GIT_SHALLOW    FALSE
)
if(POLICY CMP0169)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(quickjs_ng)
if(NOT quickjs_ng_POPULATED)
    FetchContent_Populate(quickjs_ng)
    add_subdirectory(
        "${quickjs_ng_SOURCE_DIR}"
        "${quickjs_ng_BINARY_DIR}"
        EXCLUDE_FROM_ALL
    )
endif()
if(POLICY CMP0169)
    cmake_policy(POP)
endif()

# Subdirectories
add_subdirectory("PrismatiXEngine")

# RuntimeCore consumes the VFS abstraction while the native VFS implementation
# lives in RuntimePlatformNative. Declare that native-only static-library cycle
# explicitly so CMake emits the required archive rescans on one-pass linkers
# such as GNU ld, instead of relying on platform-specific linker behavior.
if(NOT EMSCRIPTEN)
    target_link_libraries(PrismatiXRuntimeCore PRIVATE
        PrismatiXRuntimePlatformNative)
endif()

# The TypeScript runtime package is authoring/build-time tooling. Ship it next
# to the Authoring SDK without making it a native Runtime dependency.
if(NOT EMSCRIPTEN)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/runtime-sdk/dist/src/"
        DESTINATION "share/PrismatiX/runtime-sdk/dist/src"
        COMPONENT PrismatiXSDK
        OPTIONAL
    )
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime-sdk/package.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/runtime-sdk/README.md"
        DESTINATION "share/PrismatiX/runtime-sdk"
        COMPONENT PrismatiXSDK
    )
endif()

# Formatting helper
file(GLOB_RECURSE ALL_CXX_SOURCE_FILES
    "PrismatiXEngine/Applications/*.cpp"
    "PrismatiXEngine/Applications/*.h"
    "PrismatiXEngine/Engine/*.cpp"
    "PrismatiXEngine/Engine/*.h"
)
add_custom_target(format
    COMMAND clang-format -i -style=file ${ALL_CXX_SOURCE_FILES}
)
