# Studio Preview is a source-built Emscripten composition. Keep this dependency
# graph separate from the native vcpkg/Homebrew packages so RuntimeCore is the
# shared layer and only the platform composition changes.
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_DEPS_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
if(PRISMATIX_WEB_RENDERER STREQUAL "webgpu")
    # SDL has no official WebGPU backend yet. The WebGPU Web Player uses the
    # unmerged libsdl-org/SDL#16020 (SDL 3.5.0-dev), pinned to one commit of
    # its source repository. Its CMake enables SDL_GPU, the GPU 2D renderer
    # and Emscripten's emdawnwebgpu port. Nothing else builds this SDL.
    set(PRISMATIX_WEB_SDL_REPOSITORY "https://github.com/TheeStickmahn/SDL_wgpu.git")
    set(PRISMATIX_WEB_SDL_COMMIT "09026288bffe2bf9d7451255b766256a34bd8963")
    # emdawnwebgpu is a "remote" port: Emscripten downloads the version its
    # tools/ports/emdawnwebgpu.py names and checks its SHA-512. Emscripten
    # 6.0.5 (the CI pin) names this one; refuse a different Dawn silently
    # arriving with another Emscripten.
    set(PRISMATIX_WEB_EMDAWNWEBGPU "v20260423.175430")
    if(EMSCRIPTEN_ROOT_PATH)
        set(prismatix_emscripten_root "${EMSCRIPTEN_ROOT_PATH}")
    else()
        get_filename_component(prismatix_emscripten_root "${CMAKE_C_COMPILER}" DIRECTORY)
    endif()
    file(STRINGS "${prismatix_emscripten_root}/tools/ports/emdawnwebgpu.py" prismatix_emdawn_version
        REGEX "^_VERSION = ")
    if(NOT prismatix_emdawn_version MATCHES "'${PRISMATIX_WEB_EMDAWNWEBGPU}'")
        message(FATAL_ERROR
            "The WebGPU Web Player is pinned to emdawnwebgpu ${PRISMATIX_WEB_EMDAWNWEBGPU} "
            "(Emscripten 6.0.5); this Emscripten has ${prismatix_emdawn_version}.")
    endif()
    set(SDL_GPU ON CACHE BOOL "" FORCE)
    set(SDL_RENDER_GPU ON CACHE BOOL "" FORCE)
    set(SDL_WEBGPU ON CACHE BOOL "" FORCE)
else()
    set(PRISMATIX_WEB_SDL_REPOSITORY "https://github.com/libsdl-org/SDL.git")
    set(PRISMATIX_WEB_SDL_COMMIT "8e37db5e797b6167f3a00d697d816a684bd259c7")
endif()
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY ${PRISMATIX_WEB_SDL_REPOSITORY}
    GIT_TAG        ${PRISMATIX_WEB_SDL_COMMIT}
    GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(SDL3)
if(PRISMATIX_WEB_RENDERER STREQUAL "webgpu" AND NOT EXISTS "${sdl3_SOURCE_DIR}/src/gpu/webgpu")
    message(FATAL_ERROR "${sdl3_SOURCE_DIR} has no SDL_GPU WebGPU backend (src/gpu/webgpu)")
endif()

# Preview currently needs deterministic PNG/JPEG loading and PNG capture. Keep
# the browser bundle free of heavyweight native image codecs.
set(SDLIMAGE_VENDORED ON CACHE BOOL "" FORCE)
set(SDLIMAGE_DEPS_SHARED OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TESTS OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_STRICT ON CACHE BOOL "" FORCE)
set(SDLIMAGE_ANI OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_BMP ON CACHE BOOL "" FORCE)
set(SDLIMAGE_GIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_JPG ON CACHE BOOL "" FORCE)
set(SDLIMAGE_JPG_SAVE OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_JXL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_LBM OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_PCX OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_PNG ON CACHE BOOL "" FORCE)
set(SDLIMAGE_PNG_LIBPNG ON CACHE BOOL "" FORCE)
set(SDLIMAGE_PNG_SAVE ON CACHE BOOL "" FORCE)
set(SDLIMAGE_PNM OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_QOI OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_SVG OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TGA OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_WEBP ${PRISMATIX_BUILD_WASM_PLAYER} CACHE BOOL "" FORCE)
set(SDLIMAGE_XCF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_XPM OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_XV OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL3_image
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_image.git
    GIT_TAG        bec9134a26c7d0f31b36d6083c25296e04cabff5
    GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(SDL3_image)

# HarfBuzz stays enabled so Preview and Native share CJK shaping semantics.
# Dependency warnings must never be promoted by PrismatiX's release gate: the
# gate owns PrismatiX targets, not vendored third-party sources. Use a recent
# pinned SDL_ttf snapshot whose vendored HarfBuzz supports current Clang rather
# than source-patching the older 3.2.2 dependency during configure.
set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
set(SDLTTF_HARFBUZZ ON CACHE BOOL "" FORCE)
set(SDLTTF_PLUTOSVG OFF CACHE BOOL "" FORCE)
set(SDLTTF_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLTTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLTTF_WERROR OFF CACHE BOOL "" FORCE)
set(SDLTTF_STRICT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL3_ttf
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
    GIT_TAG        a42434b8c96daaf7650dbd0befe480c090d1c2eb
    GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(SDL3_ttf)

# HarfBuzz's hb.hh promotes the whole -Wunused group to errors internally.
# Clang 24 moved -Wunused-template under that umbrella, so command-line
# -Wno-error=unused-template alone cannot win after the source pragma runs.
# Disable HarfBuzz's own error-promotion block for this vendored dependency;
# PrismatiX targets still use the normal warnings-as-errors policy.
foreach(prismatix_harfbuzz_target IN ITEMS harfbuzz harfbuzz-subset)
    if(TARGET ${prismatix_harfbuzz_target} AND
       CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_definitions(${prismatix_harfbuzz_target} PRIVATE
            HB_NO_PRAGMA_GCC_DIAGNOSTIC_ERROR=1)
        target_compile_options(${prismatix_harfbuzz_target} PRIVATE
            -Wno-error
            -Wno-error=unused-template
            -Wno-unused-template
        )
    endif()
endforeach()

# Browser audio uses codecs that compile without native dynamic libraries.
set(SDLMIXER_VENDORED ON CACHE BOOL "" FORCE)
set(SDLMIXER_DEPS_SHARED OFF CACHE BOOL "" FORCE)
set(SDLMIXER_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLMIXER_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDLMIXER_TESTS OFF CACHE BOOL "" FORCE)
set(SDLMIXER_STRICT ON CACHE BOOL "" FORCE)
set(SDLMIXER_FLAC ON CACHE BOOL "" FORCE)
set(SDLMIXER_FLAC_DRFLAC ON CACHE BOOL "" FORCE)
set(SDLMIXER_FLAC_LIBFLAC OFF CACHE BOOL "" FORCE)
set(SDLMIXER_GME OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MIDI OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MOD OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MP3 ON CACHE BOOL "" FORCE)
set(SDLMIXER_MP3_DRMP3 ON CACHE BOOL "" FORCE)
set(SDLMIXER_MP3_MPG123 OFF CACHE BOOL "" FORCE)
set(SDLMIXER_OPUS ${PRISMATIX_BUILD_WASM_PLAYER} CACHE BOOL "" FORCE)
set(SDLMIXER_VORBIS_STB ON CACHE BOOL "" FORCE)
set(SDLMIXER_VORBIS_TREMOR OFF CACHE BOOL "" FORCE)
set(SDLMIXER_VORBIS_VORBISFILE OFF CACHE BOOL "" FORCE)
set(SDLMIXER_WAVPACK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL3_mixer
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_mixer.git
    GIT_TAG        72a81869b45e249e8e67102db4e98dd2441f05a1
    GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(SDL3_mixer)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        65ee68451d8eb2b5f3a30b410476ab83deb3289b
    GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(nlohmann_json)

set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        79524ddd08a4ec981b7fea76afd08ee05f83755d
    GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(spdlog)

# Native and browser Preview must use the same Unicode algorithms. Build the
# small Unicode libraries from pinned source instead of silently selecting the
# byte-oriented Runtime fallbacks under Emscripten.
set(UTF8PROC_INSTALL OFF CACHE BOOL "" FORCE)
set(UTF8PROC_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    utf8proc
    GIT_REPOSITORY https://github.com/JuliaStrings/utf8proc.git
    GIT_TAG        e5e799221b45bbb90f5fdc5c69b6b8dfbf017e78
    GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(utf8proc)
if(TARGET utf8proc AND NOT TARGET utf8proc::utf8proc)
    add_library(utf8proc::utf8proc ALIAS utf8proc)
endif()

FetchContent_Declare(
    prismatix_unibreak_source
    URL      https://github.com/adah1972/libunibreak/releases/download/libunibreak_6_1/libunibreak-6.1.tar.gz
    URL_HASH SHA256=cc4de0099cf7ff05005ceabff4afed4c582a736abc38033e70fdac86335ce93f
)
FetchContent_GetProperties(prismatix_unibreak_source)
if(NOT prismatix_unibreak_source_POPULATED)
    FetchContent_Populate(prismatix_unibreak_source)
endif()
add_library(prismatix_unibreak_wasm STATIC
    ${prismatix_unibreak_source_SOURCE_DIR}/src/unibreakbase.c
    ${prismatix_unibreak_source_SOURCE_DIR}/src/unibreakdef.c
    ${prismatix_unibreak_source_SOURCE_DIR}/src/linebreak.c
    ${prismatix_unibreak_source_SOURCE_DIR}/src/linebreakdata.c
    ${prismatix_unibreak_source_SOURCE_DIR}/src/linebreakdef.c
    ${prismatix_unibreak_source_SOURCE_DIR}/src/eastasianwidthdef.c
    ${prismatix_unibreak_source_SOURCE_DIR}/src/emojidef.c
    ${prismatix_unibreak_source_SOURCE_DIR}/src/graphemebreak.c
    ${prismatix_unibreak_source_SOURCE_DIR}/src/wordbreak.c
)
target_include_directories(prismatix_unibreak_wasm PUBLIC
    ${prismatix_unibreak_source_SOURCE_DIR}/src)
add_library(libunibreak::libunibreak ALIAS prismatix_unibreak_wasm)

# FriBidi's release build generates Unicode tables with a build-machine C
# compiler. Its Autotools distribution already separates CC_FOR_BUILD from
# the target compiler, which is the safe cross-compilation path for WASM.
include(ExternalProject)
find_program(PRISMATIX_WASM_HOST_CC NAMES cc clang gcc REQUIRED)
get_filename_component(PRISMATIX_WASM_HOST_BIN
    "${PRISMATIX_WASM_HOST_CC}" DIRECTORY)
unset(PRISMATIX_WASM_MAKE_EXECUTABLE CACHE)
find_program(PRISMATIX_WASM_MAKE_EXECUTABLE NAMES make gmake
    HINTS "${PRISMATIX_WASM_HOST_BIN}" NO_DEFAULT_PATH)
if(NOT PRISMATIX_WASM_MAKE_EXECUTABLE)
    find_program(PRISMATIX_WASM_MAKE_EXECUTABLE NAMES make gmake REQUIRED)
endif()
unset(PRISMATIX_WASM_SH_EXECUTABLE CACHE)
find_program(PRISMATIX_WASM_SH_EXECUTABLE NAMES sh bash
    HINTS
        "$ENV{ProgramFiles}/Git/bin"
        "$ENV{ProgramW6432}/Git/bin"
        "${PRISMATIX_WASM_HOST_BIN}"
    NO_DEFAULT_PATH)
if(NOT PRISMATIX_WASM_SH_EXECUTABLE)
    find_program(PRISMATIX_WASM_SH_EXECUTABLE NAMES sh bash REQUIRED)
endif()
set(PRISMATIX_WASM_FRIBIDI_INSTALL
    "${CMAKE_BINARY_DIR}/wasm-deps/fribidi")
set(PRISMATIX_WASM_FRIBIDI_SOURCE
    "${CMAKE_BINARY_DIR}/wasm-deps/fribidi-build/src/prismatix_fribidi_external")
set(PRISMATIX_WASM_FRIBIDI_BINARY
    "${CMAKE_BINARY_DIR}/wasm-deps/fribidi-build/src/prismatix_fribidi_external-build")
set(PRISMATIX_WASM_FRIBIDI_CONFIGURE_SOURCE
    "${PRISMATIX_WASM_FRIBIDI_SOURCE}")
set(PRISMATIX_WASM_FRIBIDI_CONFIGURE_INSTALL
    "${PRISMATIX_WASM_FRIBIDI_INSTALL}")
set(PRISMATIX_WASM_FRIBIDI_CONFIGURE_BINARY
    "${PRISMATIX_WASM_FRIBIDI_BINARY}")
set(PRISMATIX_WASM_HOST_BIN_SHELL "${PRISMATIX_WASM_HOST_BIN}")
if(CMAKE_HOST_WIN32)
    foreach(PRISMATIX_WASM_PATH_VARIABLE IN ITEMS
            PRISMATIX_WASM_FRIBIDI_CONFIGURE_SOURCE
            PRISMATIX_WASM_FRIBIDI_CONFIGURE_INSTALL
            PRISMATIX_WASM_FRIBIDI_CONFIGURE_BINARY
            PRISMATIX_WASM_HOST_BIN_SHELL)
        string(REGEX MATCH "^([A-Za-z]):/(.*)$" PRISMATIX_WASM_PATH_MATCH
            "${${PRISMATIX_WASM_PATH_VARIABLE}}")
        if(PRISMATIX_WASM_PATH_MATCH)
            string(TOLOWER "${CMAKE_MATCH_1}" PRISMATIX_WASM_PATH_DRIVE)
            set(${PRISMATIX_WASM_PATH_VARIABLE}
                "/${PRISMATIX_WASM_PATH_DRIVE}/${CMAKE_MATCH_2}")
        endif()
    endforeach()
endif()
file(MAKE_DIRECTORY "${PRISMATIX_WASM_FRIBIDI_INSTALL}/include")
ExternalProject_Add(prismatix_fribidi_external
    URL      https://github.com/fribidi/fribidi/releases/download/v1.0.16/fribidi-1.0.16.tar.xz
    URL_HASH SHA256=1b1cde5b235d40479e91be2f0e88a309e3214c8ab470ec8a2744d82a5a9ea05c
    PREFIX "${CMAKE_BINARY_DIR}/wasm-deps/fribidi-build"
    SOURCE_DIR "${PRISMATIX_WASM_FRIBIDI_SOURCE}"
    BINARY_DIR "${PRISMATIX_WASM_FRIBIDI_BINARY}"
    CONFIGURE_COMMAND
        ${CMAKE_COMMAND} -E env
            "CC=${CMAKE_C_COMPILER}"
            "AR=${CMAKE_AR}"
            "RANLIB=${CMAKE_RANLIB}"
            "NM=${CMAKE_NM}"
            "CC_FOR_BUILD=${PRISMATIX_WASM_HOST_CC}"
        ${PRISMATIX_WASM_SH_EXECUTABLE}
            "${PRISMATIX_WASM_FRIBIDI_CONFIGURE_SOURCE}/configure"
            --host=wasm32-unknown-emscripten
            --prefix=${PRISMATIX_WASM_FRIBIDI_CONFIGURE_INSTALL}
            --disable-shared
            --enable-static
            --disable-docs
            --disable-bin
            --disable-dependency-tracking
    BUILD_COMMAND
        ${PRISMATIX_WASM_SH_EXECUTABLE} -c
            "PATH='${PRISMATIX_WASM_HOST_BIN_SHELL}':$PATH CC='${CMAKE_C_COMPILER}' AR='${CMAKE_AR}' RANLIB='${CMAKE_RANLIB}' make -C '${PRISMATIX_WASM_FRIBIDI_CONFIGURE_BINARY}' -j2"
    INSTALL_COMMAND
        ${PRISMATIX_WASM_SH_EXECUTABLE} -c
            "PATH='${PRISMATIX_WASM_HOST_BIN_SHELL}':$PATH CC='${CMAKE_C_COMPILER}' AR='${CMAKE_AR}' RANLIB='${CMAKE_RANLIB}' make -C '${PRISMATIX_WASM_FRIBIDI_CONFIGURE_BINARY}' install"
    BUILD_BYPRODUCTS
        "${PRISMATIX_WASM_FRIBIDI_INSTALL}/lib/libfribidi.a"
)
add_library(prismatix_fribidi_wasm STATIC IMPORTED GLOBAL)
set_target_properties(prismatix_fribidi_wasm PROPERTIES
    IMPORTED_LOCATION
        "${PRISMATIX_WASM_FRIBIDI_INSTALL}/lib/libfribidi.a"
    INTERFACE_INCLUDE_DIRECTORIES
        "${PRISMATIX_WASM_FRIBIDI_INSTALL}/include")
add_dependencies(prismatix_fribidi_wasm prismatix_fribidi_external)

# The shipping Player keeps the native PDX, signature and save contracts. These
# libraries are compiled for WASM; native packages are never linked into it.
if(PRISMATIX_BUILD_WASM_PLAYER)
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(prismatix_mbedtls
        GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
        GIT_TAG v3.6.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(prismatix_mbedtls)
    add_library(prismatix_mbedcrypto INTERFACE)
    target_link_libraries(prismatix_mbedcrypto INTERFACE mbedcrypto)

    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(prismatix_zstd
        GIT_REPOSITORY https://github.com/facebook/zstd.git
        GIT_TAG v1.5.7
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR build/cmake)
    FetchContent_MakeAvailable(prismatix_zstd)
    add_library(zstd::libzstd ALIAS libzstd_static)
endif()
