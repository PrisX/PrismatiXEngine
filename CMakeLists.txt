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
                "${_PRISMATIX_HOMEBREW_PREFIX}/opt/lua@5.4"
            )
        endforeach()
    endif()
endif()

add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Dependencies (vcpkg manifest on Windows, Homebrew packages on macOS)
find_package(SDL3 CONFIG REQUIRED)
find_package(SDL3_image CONFIG REQUIRED)
find_package(SDL3_ttf CONFIG REQUIRED)
find_package(SDL3_mixer CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(MbedTLS CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(FFMPEG REQUIRED)

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

# Lua + sol2
include(FetchContent)
FetchContent_Declare(
    sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2.git
    # Exact revision already exercised by the Windows/Linux/macOS build API.
    # Do not replace this with a mutable branch such as develop.
    GIT_TAG        c1f95a773c6f8f4fde8ca3efe872e7286afe4444
    GIT_SHALLOW    FALSE
)
FetchContent_MakeAvailable(sol2)
find_package(Lua 5.4 REQUIRED)

# Subdirectories
add_subdirectory("PrismatiXEngine")

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
