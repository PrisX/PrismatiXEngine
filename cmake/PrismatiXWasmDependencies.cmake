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
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        8e37db5e797b6167f3a00d697d816a684bd259c7
    GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(SDL3)

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
set(SDLIMAGE_WEBP OFF CACHE BOOL "" FORCE)
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
set(SDLMIXER_OPUS OFF CACHE BOOL "" FORCE)
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
find_program(PRISMATIX_WASM_MAKE_EXECUTABLE NAMES make gmake REQUIRED)
find_program(PRISMATIX_WASM_HOST_CC NAMES cc clang gcc REQUIRED)
set(PRISMATIX_WASM_FRIBIDI_INSTALL
    "${CMAKE_BINARY_DIR}/wasm-deps/fribidi")
file(MAKE_DIRECTORY "${PRISMATIX_WASM_FRIBIDI_INSTALL}/include")
ExternalProject_Add(prismatix_fribidi_external
    URL      https://github.com/fribidi/fribidi/releases/download/v1.0.16/fribidi-1.0.16.tar.xz
    URL_HASH SHA256=1b1cde5b235d40479e91be2f0e88a309e3214c8ab470ec8a2744d82a5a9ea05c
    PREFIX "${CMAKE_BINARY_DIR}/wasm-deps/fribidi-build"
    CONFIGURE_COMMAND
        ${CMAKE_COMMAND} -E env
            "CC=${CMAKE_C_COMPILER}"
            "AR=${CMAKE_AR}"
            "RANLIB=${CMAKE_RANLIB}"
            "NM=${CMAKE_NM}"
            "CC_FOR_BUILD=${PRISMATIX_WASM_HOST_CC}"
        <SOURCE_DIR>/configure
            --host=wasm32-unknown-emscripten
            --prefix=${PRISMATIX_WASM_FRIBIDI_INSTALL}
            --disable-shared
            --enable-static
            --disable-docs
            --disable-bin
            --disable-dependency-tracking
    BUILD_COMMAND
        ${CMAKE_COMMAND} -E env
            "CC=${CMAKE_C_COMPILER}"
            "AR=${CMAKE_AR}"
            "RANLIB=${CMAKE_RANLIB}"
        ${PRISMATIX_WASM_MAKE_EXECUTABLE} -C <BINARY_DIR> -j2
    INSTALL_COMMAND
        ${CMAKE_COMMAND} -E env
            "CC=${CMAKE_C_COMPILER}"
            "AR=${CMAKE_AR}"
            "RANLIB=${CMAKE_RANLIB}"
        ${PRISMATIX_WASM_MAKE_EXECUTABLE} -C <BINARY_DIR> install
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
