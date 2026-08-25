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
# gate owns PrismatiX targets, not vendored third-party sources.
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
    GIT_TAG        a1ce3670aec736ecbf0936c43f2f0cc53aa61e5b
    GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(SDL3_ttf)
# The pinned SDL_ttf snapshot vendors HarfBuzz with its own warning policy.
# Appending -Wno-error after that target is created keeps warnings visible while
# preventing upstream diagnostics from failing PrismatiX's -Werror gate.
if(TARGET harfbuzz AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(harfbuzz PRIVATE -Wno-error)
endif()

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
