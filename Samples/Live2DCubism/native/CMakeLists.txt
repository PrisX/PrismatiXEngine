# Builds the Live2D Cubism native extension for the platform you run on:
#   cmake -S native -B build/native -DPX_ENGINE_INCLUDE=<repo>/PrismatiXEngine
#   cmake --build build/native
# The libraries land in extension/native/<platform>/ where the manifest expects them.
cmake_minimum_required(VERSION 3.21)
project(px_live2d C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)

# The folder that holds Engine/Native/PxPluginAbi.h (PrismatiXEngine/PrismatiXEngine in the engine repository).
set(PX_ENGINE_INCLUDE "" CACHE PATH "Folder containing Engine/Native/PxPluginAbi.h")
if(NOT EXISTS "${PX_ENGINE_INCLUDE}/Engine/Native/PxPluginAbi.h")
    message(FATAL_ERROR "Set PX_ENGINE_INCLUDE to the folder containing Engine/Native/PxPluginAbi.h")
endif()

if(WIN32)
    set(PX_PLATFORM "win32-x64")
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(PX_PLATFORM "darwin-arm64")
    else()
        set(PX_PLATFORM "darwin-x64")
    endif()
else()
    set(PX_PLATFORM "linux-x64")
endif()
set(PX_OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/../extension/native/${PX_PLATFORM}")

add_library(px_live2d SHARED px_live2d_plugin.c cubism_bridge.c)
target_include_directories(px_live2d PRIVATE "${PX_ENGINE_INCLUDE}")
target_link_libraries(px_live2d PRIVATE ${CMAKE_DL_LIBS})
if(NOT WIN32)
    target_link_libraries(px_live2d PRIVATE m)
endif()
set_target_properties(px_live2d PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${PX_OUTPUT}"
    RUNTIME_OUTPUT_DIRECTORY "${PX_OUTPUT}"
    PREFIX "lib")
if(WIN32)
    set_target_properties(px_live2d PROPERTIES PREFIX "")
endif()
if(MSVC)
    target_compile_options(px_live2d PRIVATE /W4)
else()
    target_compile_options(px_live2d PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Run this against your own Core and a real .moc3 before using the plugin.
add_executable(cubism_smoke cubism_smoke.c cubism_bridge.c)
target_link_libraries(cubism_smoke PRIVATE ${CMAKE_DL_LIBS})
if(NOT WIN32)
    target_link_libraries(cubism_smoke PRIVATE m)
endif()

option(PX_LIVE2D_TESTS "Build the mock Core and the plugin harness" OFF)
if(PX_LIVE2D_TESTS)
    add_library(mock_cubism_core SHARED test/mock_cubism_core.c)
    set_target_properties(mock_cubism_core PROPERTIES PREFIX "lib")
    add_executable(plugin_harness test/plugin_harness.c)
    target_include_directories(plugin_harness PRIVATE "${PX_ENGINE_INCLUDE}")
    target_link_libraries(plugin_harness PRIVATE ${CMAKE_DL_LIBS})
endif()
