# Top-level CMake project for PrismatiXEngine
cmake_minimum_required (VERSION 3.21)

if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("PrismatiXEngine" LANGUAGES C CXX)

if(APPLE)
    execute_process(COMMAND brew --prefix OUTPUT_VARIABLE HOMEBREW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE)
    list(APPEND CMAKE_PREFIX_PATH
        "${HOMEBREW_PREFIX}"
        "/opt/homebrew/opt/lua@5.4"
    )
endif()

add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Dependencies (vcpkg manifest)
find_package(SDL3 CONFIG REQUIRED)
find_package(SDL3_image CONFIG REQUIRED)
find_package(SDL3_ttf CONFIG REQUIRED)
find_package(SDL3_mixer CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(MbedTLS CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)

# Lua + sol2
include(FetchContent)
FetchContent_Declare(
    sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2.git
    GIT_TAG        develop
)
FetchContent_MakeAvailable(sol2)
find_package(Lua REQUIRED)

# Subdirectories
add_subdirectory("ThirdParty")
add_subdirectory("PrismatiXEngine")

# Formatting helper
file(GLOB_RECURSE ALL_CXX_SOURCE_FILES
    "PrismatiXEngine/Engine/*.cpp"
    "PrismatiXEngine/Engine/*.h"
    "PrismatiXEngine/Editor/*.cpp"
    "PrismatiXEngine/Editor/*.h"
)
add_custom_target(format
    COMMAND clang-format -i -style=file ${ALL_CXX_SOURCE_FILES}
)
