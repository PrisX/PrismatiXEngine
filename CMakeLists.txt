# Top-level CMake project for PrismatiXEngine
cmake_minimum_required (VERSION 3.21)

if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<CONFIG:Debug>,EditAndContinue,$<$<CONFIG:RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("PrismatiXEngine" LANGUAGES C CXX)

include(CTest)
option(PRISMATIX_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(PRISMATIX_WARNINGS_AS_ERRORS "Treat compiler warnings as release-blocking errors" OFF)
option(PRISMATIX_BUILD_FUZZERS "Build libFuzzer parser targets" OFF)
option(PRISMATIX_PERFORMANCE_TESTS "Enable scheduled large-scene performance scenarios" OFF)
option(PRISMATIX_STRESS_TESTS "Enable long-running performance and 50,000-asset stress tests" OFF)
option(PRISMATIX_BUILD_WASM_PREVIEW "Build the Studio-only Emscripten preview runtime" OFF)

if(PRISMATIX_BUILD_WASM_PREVIEW AND NOT EMSCRIPTEN)
    message(FATAL_ERROR
        "PRISMATIX_BUILD_WASM_PREVIEW requires the Emscripten toolchain (configure with emcmake).")
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
        # Clang 24's Emscripten toolchain diagnoses intentionally partial C++20
        # designated initializers more aggressively than the native compilers.
        # Keep the diagnostic visible without making this safe default-
        # initialization pattern release-blocking; all other warnings remain
        # errors under PRISMATIX_WARNINGS_AS_ERRORS.
        if(EMSCRIPTEN AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE
                -Wno-error=missing-designated-field-initializers)
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
    find_package(SDL3_mixer CONFIG REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
    find_package(spdlog CONFIG REQUIRED)
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
    "PrismatiXEngine/Editor/*.cpp"
    "PrismatiXEngine/Editor/*.h"
)
add_custom_target(format
    COMMAND clang-format -i -style=file ${ALL_CXX_SOURCE_FILES}
)
