cmake_minimum_required(VERSION 3.21)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/PrismatiXContractVersions.cmake")

set(PRISMATIX_GENERATED_INCLUDE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${PRISMATIX_GENERATED_INCLUDE_DIR}/Engine/SDK")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/SDK/ContractVersions.h.in"
    "${PRISMATIX_GENERATED_INCLUDE_DIR}/Engine/SDK/ContractVersions.h"
    @ONLY
)

execute_process(
    COMMAND git -c safe.directory=${CMAKE_SOURCE_DIR} rev-parse HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE PRISMATIX_ENGINE_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
string(LENGTH "${PRISMATIX_ENGINE_COMMIT}" PRISMATIX_ENGINE_COMMIT_LENGTH)
if(NOT PRISMATIX_ENGINE_COMMIT_LENGTH EQUAL 40 OR
   NOT PRISMATIX_ENGINE_COMMIT MATCHES "^[0-9a-fA-F]+$")
    set(PRISMATIX_ENGINE_COMMIT "0000000000000000000000000000000000000000")
endif()
if(NOT DEFINED PRISMATIX_TARGET_TRIPLE OR PRISMATIX_TARGET_TRIPLE STREQUAL "")
    if(ANDROID AND CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
        set(PRISMATIX_TARGET_TRIPLE "aarch64-linux-android")
    elseif(ANDROID)
        set(PRISMATIX_TARGET_TRIPLE "${CMAKE_ANDROID_ARCH_ABI}-linux-android")
    elseif(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(PRISMATIX_TARGET_TRIPLE "x86_64-pc-windows-msvc")
    elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        set(PRISMATIX_TARGET_TRIPLE "aarch64-apple-darwin")
    elseif(APPLE)
        set(PRISMATIX_TARGET_TRIPLE "x86_64-apple-darwin")
    elseif(UNIX AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        set(PRISMATIX_TARGET_TRIPLE "aarch64-unknown-linux-gnu")
    else()
        set(PRISMATIX_TARGET_TRIPLE "x86_64-unknown-linux-gnu")
    endif()
endif()
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PrismatiXSDKManifest.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PrismatiXSDKManifest.json"
    @ONLY
)
# The build tree carries its own manifest, so `--runtime out/build/<preset>/
# PrismatiXEngine` works without an install; the installed one points from
# share/PrismatiX to bin.
set(PRISMATIX_RUNTIME_BIN_DIR ".")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PrismatiXRuntimeManifest.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/runtime-manifest.json"
    @ONLY
)
set(PRISMATIX_RUNTIME_BIN_DIR "../../bin")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PrismatiXRuntimeManifest.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/install/runtime-manifest.json"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PrismatiXReleaseInventory.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/release-inventory.json"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PreviewContractManifest.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/preview-contract.json"
    @ONLY
)

add_library(PrismatiXRuntimeContracts STATIC
    Engine/Core/SemanticVersion.cpp
    Engine/SDK/CharacterResources.cpp
    Engine/SDK/FontIntegrity.cpp
    Engine/SDK/GameCatalogResources.cpp
    Engine/SDK/Motion.cpp
    Engine/SDK/Portrait.cpp
    Engine/SDK/PackageManifest.cpp
    Engine/SDK/RuntimeIr.cpp
    Engine/SDK/SourceMap.cpp
    Engine/SDK/Ui.cpp
    Engine/SDK/UiJson.cpp
)
add_library(PrismatiX::RuntimeContracts ALIAS PrismatiXRuntimeContracts)
set_target_properties(PrismatiXRuntimeContracts PROPERTIES
    EXPORT_NAME RuntimeContracts)
prismatix_configure_target(PrismatiXRuntimeContracts)
target_include_directories(PrismatiXRuntimeContracts PUBLIC
    $<BUILD_INTERFACE:${PRISMATIX_GENERATED_INCLUDE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/PrismatiX>
)
target_link_libraries(PrismatiXRuntimeContracts PUBLIC
    nlohmann_json::nlohmann_json
)

if(NOT EMSCRIPTEN)
add_library(PrismatiXSDK STATIC
    Engine/SDK/Packager.cpp
    Engine/SDK/PackageBranding.cpp
    Engine/SDK/UiTypeRegistry.cpp
)
add_library(PrismatiX::SDK ALIAS PrismatiXSDK)
set_target_properties(PrismatiXSDK PROPERTIES EXPORT_NAME SDK)
prismatix_configure_target(PrismatiXSDK)
target_include_directories(PrismatiXSDK PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/PrismatiX>
)
target_link_libraries(PrismatiXSDK PUBLIC
    PrismatiXRuntimeContracts
    zstd::libzstd
    SDL3::SDL3
    SDL3_ttf::SDL3_ttf
)
if(ANDROID)
    # Packaging (and so custom-effect shader compilation) runs on the host.
    # The Android Player links PrismatiXSDK only for package verification.
    target_compile_definitions(PrismatiXSDK PRIVATE
        PRISMATIX_OFFLINE_SHADER_COMPILER=0)
else()
    target_link_libraries(PrismatiXSDK PUBLIC
        SDL3_shadercross::SDL3_shadercross)
    target_compile_definitions(PrismatiXSDK PRIVATE
        PRISMATIX_OFFLINE_SHADER_COMPILER=1)
endif()
if(TARGET MbedTLS::mbedcrypto)
    target_link_libraries(PrismatiXSDK PUBLIC MbedTLS::mbedcrypto)
elseif(TARGET MbedTLS::tfpsacrypto)
    target_link_libraries(PrismatiXSDK PUBLIC MbedTLS::tfpsacrypto)
elseif(TARGET MbedTLS::mbedtls)
    target_link_libraries(PrismatiXSDK PUBLIC MbedTLS::mbedtls)
endif()

add_library(PrismatiXPackagerSDK INTERFACE)
add_library(PrismatiX::PackagerSDK ALIAS PrismatiXPackagerSDK)
set_target_properties(PrismatiXPackagerSDK PROPERTIES EXPORT_NAME PackagerSDK)
target_link_libraries(PrismatiXPackagerSDK INTERFACE PrismatiXSDK)
if(NOT ANDROID)
    target_link_libraries(PrismatiXPackagerSDK INTERFACE
        SDL3_shadercross::SDL3_shadercross)
endif()

install(TARGETS PrismatiXSDK PrismatiXPackagerSDK EXPORT PrismatiXSDKTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT PrismatiXSDK
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT PrismatiXSDK
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT PrismatiXSDK
)
install(TARGETS PrismatiXRuntimeContracts EXPORT PrismatiXSDKTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT PrismatiXSDK
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT PrismatiXSDK
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT PrismatiXSDK
)
install(DIRECTORY Engine/SDK
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/PrismatiX/Engine
    COMPONENT PrismatiXSDK
    FILES_MATCHING PATTERN "*.h"
)
# The C ABI native extension libraries build against (docs/native-extensions.md).
install(FILES Engine/Native/PxPluginAbi.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/PrismatiX
    COMPONENT PrismatiXSDK
)
install(FILES
    "${PRISMATIX_GENERATED_INCLUDE_DIR}/Engine/SDK/ContractVersions.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/PrismatiX/Engine/SDK
    COMPONENT PrismatiXSDK
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PrismatiXSDKConfigVersion.cmake"
    VERSION ${PRISMATIX_ENGINE_VERSION}
    COMPATIBILITY SameMinorVersion
)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PrismatiXSDKConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/PrismatiXSDKConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PrismatiXSDK
)
install(EXPORT PrismatiXSDKTargets
    FILE PrismatiXSDKTargets.cmake
    NAMESPACE PrismatiX::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PrismatiXSDK
    COMPONENT PrismatiXSDK
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/PrismatiXSDKConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/PrismatiXSDKConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PrismatiXSDK
    COMPONENT PrismatiXSDK
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/PrismatiXSDKManifest.json"
    DESTINATION .
    COMPONENT PrismatiXSDK
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../Contracts/"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/PrismatiX/contracts
    COMPONENT PrismatiXSDK
    FILES_MATCHING PATTERN "*.json"
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../packages/authoring-sdk/dist/src/"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/PrismatiX/authoring-sdk/dist/src
    COMPONENT PrismatiXSDK
    OPTIONAL
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/../packages/authoring-sdk/package.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/../packages/authoring-sdk/README.md"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/PrismatiX/authoring-sdk
    COMPONENT PrismatiXSDK
)
endif()


file(GLOB_RECURSE PRISMATIX_RUNTIME_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/*.cpp"
)

if(NOT EMSCRIPTEN)
    find_program(PRISMATIX_SHADERCROSS_EXECUTABLE
        NAMES shadercross
        REQUIRED
    )
    set(PRISMATIX_BUILTIN_SHADER_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")
    # Offline-compiles one built-in fragment shader and embeds every
    # artifact as SelectBuiltIn<name>Shader().
    function(prismatix_builtin_shader name stem)
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Builtin/${stem}.frag.hlsl")
        set(spirv "${PRISMATIX_BUILTIN_SHADER_DIR}/${stem}.frag.spv")
        set(dxil "${PRISMATIX_BUILTIN_SHADER_DIR}/${stem}.frag.dxil")
        set(msl "${PRISMATIX_BUILTIN_SHADER_DIR}/${stem}.frag.msl")
        set(generated "${PRISMATIX_BUILTIN_SHADER_DIR}/BuiltIn${name}Shaders.generated.cpp")
        if(APPLE)
            add_custom_command(
                OUTPUT "${spirv}" "${msl}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${PRISMATIX_BUILTIN_SHADER_DIR}"
                COMMAND "${PRISMATIX_SHADERCROSS_EXECUTABLE}" "${source}"
                        --source HLSL --dest SPIRV --stage fragment
                        --entrypoint main --output "${spirv}"
                COMMAND "${PRISMATIX_SHADERCROSS_EXECUTABLE}" "${spirv}"
                        --source SPIRV --dest MSL --stage fragment
                        --entrypoint main --output "${msl}"
                DEPENDS "${source}"
                COMMENT "Offline-compiling PrismatiX built-in ${stem} shader"
                VERBATIM)
            set(inputs -DINPUT_SPIRV=${spirv} -DINPUT_MSL=${msl})
            set(artifacts "${spirv}" "${msl}")
        else()
            add_custom_command(
                OUTPUT "${spirv}" "${dxil}" "${msl}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${PRISMATIX_BUILTIN_SHADER_DIR}"
                COMMAND "${PRISMATIX_SHADERCROSS_EXECUTABLE}" "${source}"
                        --source HLSL --dest SPIRV --stage fragment
                        --entrypoint main --output "${spirv}"
                COMMAND "${PRISMATIX_SHADERCROSS_EXECUTABLE}" "${source}"
                        --source HLSL --dest DXIL --stage fragment
                        --entrypoint main --output "${dxil}"
                COMMAND "${PRISMATIX_SHADERCROSS_EXECUTABLE}" "${source}"
                        --source HLSL --dest MSL --stage fragment
                        --entrypoint main --output "${msl}"
                DEPENDS "${source}"
                COMMENT "Offline-compiling PrismatiX built-in ${stem} shader"
                VERBATIM)
            set(inputs -DINPUT_SPIRV=${spirv} -DINPUT_DXIL=${dxil} -DINPUT_MSL=${msl})
            set(artifacts "${spirv}" "${dxil}" "${msl}")
        endif()
        add_custom_command(
            OUTPUT "${generated}"
            COMMAND "${CMAKE_COMMAND}" -DNAME=${name} ${inputs}
                    -DOUTPUT=${generated}
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/GenerateBuiltInShaders.cmake"
            DEPENDS ${artifacts}
                "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/GenerateBuiltInShaders.cmake"
            COMMENT "Embedding offline-compiled PrismatiX ${stem} shaders"
            VERBATIM)
        set_source_files_properties("${generated}" PROPERTIES GENERATED TRUE)
        set(PRISMATIX_RUNTIME_CORE_SOURCES ${PRISMATIX_RUNTIME_CORE_SOURCES}
            "${generated}" PARENT_SCOPE)
    endfunction()
    prismatix_builtin_shader(Compositor compositor)
    prismatix_builtin_shader(Rule rule)
endif()
list(FILTER PRISMATIX_RUNTIME_CORE_SOURCES EXCLUDE REGEX "/Engine/SDK/")
list(FILTER PRISMATIX_RUNTIME_CORE_SOURCES EXCLUDE REGEX "/Engine/Core/SemanticVersion\.cpp$")
list(FILTER PRISMATIX_RUNTIME_CORE_SOURCES EXCLUDE REGEX "/Engine/Runtime\\.cpp$")
list(FILTER PRISMATIX_RUNTIME_CORE_SOURCES EXCLUDE REGEX "/Engine/Platform/")
list(FILTER PRISMATIX_RUNTIME_CORE_SOURCES EXCLUDE REGEX "/Engine/Accessibility/SpeechService\\.cpp$")
list(FILTER PRISMATIX_RUNTIME_CORE_SOURCES EXCLUDE REGEX "/Engine/Accessibility/PlatformSemanticAdapter\\.cpp$")
list(FILTER PRISMATIX_RUNTIME_CORE_SOURCES EXCLUDE REGEX "/Engine/Accessibility/PlatformSemanticAdapterMac\\.mm$")
list(FILTER PRISMATIX_RUNTIME_CORE_SOURCES EXCLUDE REGEX "/Engine/Accessibility/PlatformSemanticAdapterLinux\\.cpp$")
list(FILTER PRISMATIX_RUNTIME_CORE_SOURCES EXCLUDE REGEX "/Engine/Video/VideoPlayer\\.cpp$")
list(FILTER PRISMATIX_RUNTIME_CORE_SOURCES EXCLUDE REGEX "/Engine/IO/")
list(FILTER PRISMATIX_RUNTIME_CORE_SOURCES EXCLUDE REGEX "/Engine/Progression/(GameSettings|GlobalProfileStore|Persist|SaveSystem)\\.cpp$")
list(FILTER PRISMATIX_RUNTIME_CORE_SOURCES EXCLUDE REGEX "/Engine/Resources/AssetRegistry\\.cpp$")
list(FILTER PRISMATIX_RUNTIME_CORE_SOURCES EXCLUDE REGEX "/Engine/UI/UITypeManifest\\.cpp$")

add_library(PrismatiXRuntimeCore STATIC ${PRISMATIX_RUNTIME_CORE_SOURCES})
add_library(PrismatiX::RuntimeCore ALIAS PrismatiXRuntimeCore)
prismatix_configure_target(PrismatiXRuntimeCore)

target_include_directories(PrismatiXRuntimeCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine
)
target_link_libraries(PrismatiXRuntimeCore PUBLIC
    SDL3::SDL3
    SDL3_image::SDL3_image
    SDL3_ttf::SDL3_ttf
    SDL3_mixer::SDL3_mixer
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    PrismatiXRuntimeContracts
)
target_link_libraries(PrismatiXRuntimeCore PRIVATE qjs)
# Native extension libraries (Engine/Native) load with dlopen outside Windows.
if(NOT EMSCRIPTEN AND CMAKE_DL_LIBS)
    target_link_libraries(PrismatiXRuntimeCore PUBLIC ${CMAKE_DL_LIBS})
endif()
if(EMSCRIPTEN)
    target_link_libraries(PrismatiXRuntimeCore PRIVATE
        utf8proc::utf8proc
        libunibreak::libunibreak
        prismatix_fribidi_wasm
    )
    add_dependencies(PrismatiXRuntimeCore prismatix_fribidi_external)
else()
    target_link_libraries(PrismatiXRuntimeCore PRIVATE
        utf8proc::utf8proc
        libunibreak::libunibreak
        PkgConfig::FRIBIDI
    )
endif()
target_compile_definitions(PrismatiXRuntimeCore PRIVATE
    PRISMATIX_UNICODE_TEXT_LIBS=1)

if(NOT EMSCRIPTEN)
    file(GLOB_RECURSE PRISMATIX_NATIVE_PLATFORM_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Engine/IO/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Platform/*.cpp"
    )
    list(APPEND PRISMATIX_NATIVE_PLATFORM_SOURCES
        Engine/Runtime.cpp
        Engine/Accessibility/SpeechService.cpp
        Engine/Accessibility/PlatformSemanticAdapter.cpp
        Engine/Video/VideoPlayer.cpp
        Engine/Resources/AssetRegistry.cpp
        Engine/UI/UITypeManifest.cpp
        Engine/Progression/GameSettings.cpp
        Engine/Progression/GlobalProfileStore.cpp
        Engine/Progression/Persist.cpp
        Engine/Progression/SaveSystem.cpp
    )
    if(APPLE)
        list(APPEND PRISMATIX_NATIVE_PLATFORM_SOURCES
            Engine/Accessibility/PlatformSemanticAdapterMac.mm)
    elseif(UNIX AND NOT ANDROID)
        list(APPEND PRISMATIX_NATIVE_PLATFORM_SOURCES
            Engine/Accessibility/PlatformSemanticAdapterLinux.cpp)
    endif()
    add_library(PrismatiXRuntimePlatformNative STATIC
        ${PRISMATIX_NATIVE_PLATFORM_SOURCES})
    add_library(PrismatiX::RuntimePlatformNative ALIAS
        PrismatiXRuntimePlatformNative)
    prismatix_configure_target(PrismatiXRuntimePlatformNative)
    target_include_directories(PrismatiXRuntimePlatformNative PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/Engine
    )
    target_include_directories(PrismatiXRuntimePlatformNative PRIVATE
        ${FFMPEG_INCLUDE_DIRS}
    )
    target_link_libraries(PrismatiXRuntimePlatformNative PUBLIC
        PrismatiXRuntimeCore
        PrismatiXSDK
        prismatix_mbedcrypto
        zstd::libzstd
        ${FFMPEG_LIBRARIES}
    )
    if(WIN32)
        target_link_libraries(PrismatiXRuntimePlatformNative PUBLIC
            uiautomationcore oleaut32 ole32)
    elseif(APPLE)
        target_link_libraries(PrismatiXRuntimePlatformNative PUBLIC
            "-framework AppKit" "-framework Foundation")
        set_source_files_properties(
            Engine/Accessibility/PlatformSemanticAdapterMac.mm
            PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    elseif(ANDROID)
        # SDL3's static CMake config also exports these; listed so a
        # static link cannot miss them.
        target_link_libraries(PrismatiXRuntimePlatformNative PUBLIC
            android log)
    elseif(UNIX)
        target_link_libraries(PrismatiXRuntimePlatformNative PUBLIC
            PkgConfig::DBUS)
    endif()

    add_library(PrismatiXRuntime INTERFACE)
    target_link_libraries(PrismatiXRuntime INTERFACE
        PrismatiXRuntimePlatformNative)
endif()

if(ANDROID)
    # FFmpeg is LGPL and must ship as replaceable shared libraries.
    set(PRISMATIX_ANDROID_SHARED_LIBRARIES)
    foreach(_library IN LISTS FFMPEG_LIBRARIES)
        if(_library MATCHES "\\.so$" AND EXISTS "${_library}")
            list(APPEND PRISMATIX_ANDROID_SHARED_LIBRARIES "${_library}")
        endif()
    endforeach()
    list(REMOVE_DUPLICATES PRISMATIX_ANDROID_SHARED_LIBRARIES)
    if(NOT PRISMATIX_ANDROID_SHARED_LIBRARIES)
        message(FATAL_ERROR "The Android Player needs FFmpeg as shared libraries "
            "(vcpkg/triplets/arm64-android-prismatix.cmake); found: ${FFMPEG_LIBRARIES}")
    endif()
    # SDLActivity refuses to start when its Java glue and the native SDL
    # differ, so catch a mismatch with the template's copy at configure time.
    set(_sdl_activity "${CMAKE_SOURCE_DIR}/packages/cli/templates/android/app/src/main/java/org/libsdl/app/SDLActivity.java")
    file(STRINGS "${_sdl_activity}" _sdl_java_versions
        REGEX "SDL_(MAJOR|MINOR|MICRO)_VERSION = [0-9]+;")
    set(_sdl_java_version)
    foreach(_part IN ITEMS MAJOR MINOR MICRO)
        string(REGEX MATCH "SDL_${_part}_VERSION = ([0-9]+)" _match "${_sdl_java_versions}")
        list(APPEND _sdl_java_version "${CMAKE_MATCH_1}")
    endforeach()
    list(JOIN _sdl_java_version "." _sdl_java_version)
    if(NOT _sdl_java_version STREQUAL SDL3_VERSION)
        message(FATAL_ERROR "Native SDL is ${SDL3_VERSION} but the Android template's "
            "Java glue is ${_sdl_java_version}. Copy android-project/app/src/main/java/org/ "
            "from the SDL ${SDL3_VERSION} source release into "
            "packages/cli/templates/android/app/src/main/java/.")
    endif()
endif()

if(NOT EMSCRIPTEN)
# PrismatiXPlayer is the shipped Player. PrismatiXPlayerQA is the same Player
# with the QA IPC test interface (Applications/Player/QaIpc.h, driven by
# `prismatix test`) compiled in; tests and QA packs use it, games never ship it.
function(prismatix_add_player target qa_hooks)
    set(_player_sources
        Applications/PlayerMain.cpp
        Applications/Player/PlayerApp.cpp
        Applications/Player/QaIpc.cpp
        Applications/Player/PlayerSettings.cpp
        Applications/Player/PlayerActions.cpp
    )
    if(ANDROID)
        # SDL's Java glue (SDLActivity) loads the game as libmain.so and
        # calls its SDL_main; see docs/shipping.md (Android). The shipped and QA Players
        # both use that name, so they build into separate directories. Each
        # directory gets everything the APK's lib/<abi>/ needs (libmain.so
        # plus the shared FFmpeg libraries) and prismatix-player.json, which
        # `prismatix pack --target android` checks.
        add_library(${target} SHARED ${_player_sources})
        if(qa_hooks)
            set(_player_variant qa)
        else()
            set(_player_variant shipped)
        endif()
        set(_player_dir
            "${CMAKE_BINARY_DIR}/android/${_player_variant}/${CMAKE_ANDROID_ARCH_ABI}")
        set_target_properties(${target} PROPERTIES
            OUTPUT_NAME main
            LIBRARY_OUTPUT_DIRECTORY "${_player_dir}")
        file(WRITE "${_player_dir}/prismatix-player.json"
            "{\"format\": \"PrismatiXAndroidPlayer\", \"variant\": \"${_player_variant}\", "
            "\"abi\": \"${CMAKE_ANDROID_ARCH_ABI}\", \"sdlVersion\": \"${SDL3_VERSION}\"}\n")
        # The APK gets a stripped libmain.so (the unstripped one is ~250 MB);
        # symbols/<abi>/libmain.so keeps the debug info for ndk-stack.
        set(_symbols_dir
            "${CMAKE_BINARY_DIR}/android/${_player_variant}/symbols/${CMAKE_ANDROID_ARCH_ABI}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_symbols_dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${target}>" "${_symbols_dir}/"
            COMMAND "${CMAKE_STRIP}" --strip-unneeded "$<TARGET_FILE:${target}>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                ${PRISMATIX_ANDROID_SHARED_LIBRARIES} "${_player_dir}"
            VERBATIM)
    else()
        add_executable(${target} ${_player_sources})
        if(WIN32 AND NOT qa_hooks)
            # A shipped game opens no console window: SDL_main provides
            # WinMain. PlayerMain.cpp attaches to a launching terminal's
            # console, and `--console` opens one (docs/shipping.md). The QA
            # Player, Packager and PreviewHost stay console programs because
            # tests and CI read their output.
            set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE TRUE)
        endif()
    endif()
    target_link_libraries(${target} PRIVATE PrismatiXRuntime)
    target_compile_definitions(${target} PRIVATE PRISMATIX_QA_HOOKS=${qa_hooks})
    if(MSVC)
        # PlayerApp intentionally integrates the complete shipped-player surface in
        # one translation unit. Async preload state can push debug builds beyond
        # COFF's default section count, so use MSVC's extended object format.
        target_compile_options(${target} PRIVATE /bigobj)
    endif()
    if(WIN32)
        target_link_libraries(${target} PRIVATE sapi)
        if(qa_hooks)
            target_link_libraries(${target} PRIVATE ws2_32)
        endif()
    endif()
    prismatix_configure_target(${target})
endfunction()
prismatix_add_player(PrismatiXPlayer 0)
prismatix_add_player(PrismatiXPlayerQA 1)

# Everything below is desktop tooling and its install layout. An Android
# build produces only the Player libraries; packaging stays on the host.
if(NOT ANDROID)
# distribution.transcode: PNG to WebP, Ogg Vorbis/WAV to Ogg Opus or Vorbis.
add_library(PrismatiXMediaTranscoder STATIC Applications/Packager/MediaTranscoder.cpp)
target_include_directories(PrismatiXMediaTranscoder PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(PrismatiXMediaTranscoder PRIVATE
    SDL3::SDL3 SDL3_image::SDL3_image nlohmann_json::nlohmann_json)
if(TARGET WebP::webp AND TARGET Ogg::ogg AND TARGET Vorbis::vorbisenc AND
   TARGET Vorbis::vorbisfile AND TARGET Opus::opus)
    target_link_libraries(PrismatiXMediaTranscoder PRIVATE
        WebP::webp Ogg::ogg Vorbis::vorbisenc Vorbis::vorbisfile Opus::opus)
    target_compile_definitions(PrismatiXMediaTranscoder PRIVATE PRISMATIX_MEDIA_ENCODERS=1)
    set(PRISMATIX_MEDIA_ENCODERS_FOUND TRUE)
else()
    message(STATUS "WebP/Vorbis/Opus encoders not found: the Packager cannot transcode media")
    target_compile_definitions(PrismatiXMediaTranscoder PRIVATE PRISMATIX_MEDIA_ENCODERS=0)
endif()
prismatix_configure_target(PrismatiXMediaTranscoder)

add_executable(PrismatiXPackager Applications/PackagerMain.cpp)
target_link_libraries(PrismatiXPackager PRIVATE PrismatiXPackagerSDK PrismatiXRuntime
    PrismatiXMediaTranscoder)
prismatix_configure_target(PrismatiXPackager)

add_executable(PrismatiXPreviewHost Applications/PreviewHostMain.cpp)
target_link_libraries(PrismatiXPreviewHost PRIVATE PrismatiXRuntime)
prismatix_configure_target(PrismatiXPreviewHost)
if(WIN32)
    target_link_libraries(PrismatiXPreviewHost PRIVATE sapi)
endif()
add_executable(PrismatiXUiTypeManifest Applications/UiTypeManifestMain.cpp)
target_link_libraries(PrismatiXUiTypeManifest PRIVATE PrismatiXRuntime)
prismatix_configure_target(PrismatiXUiTypeManifest)
set(PRISMATIX_UI_TYPE_MANIFEST
    "${CMAKE_CURRENT_BINARY_DIR}/PrismatiXUiTypeRegistry.json")
add_custom_command(
    OUTPUT "${PRISMATIX_UI_TYPE_MANIFEST}"
    COMMAND $<TARGET_FILE:PrismatiXUiTypeManifest>
            "${PRISMATIX_UI_TYPE_MANIFEST}"
    DEPENDS PrismatiXUiTypeManifest
    COMMENT "Exporting the versioned UI TypeRegistry contract"
    VERBATIM
)
add_custom_target(PrismatiXUiTypeRegistryManifest ALL
                  DEPENDS "${PRISMATIX_UI_TYPE_MANIFEST}")
# Building a single native tool must refresh the UI contract that the CLI
# discovers beside it. Otherwise an old JSON manifest can reject properties
# supported by the newly linked Player/PreviewHost.
add_dependencies(PrismatiXPackager PrismatiXUiTypeRegistryManifest)
add_dependencies(PrismatiXPlayer PrismatiXUiTypeRegistryManifest)
add_dependencies(PrismatiXPlayerQA PrismatiXUiTypeRegistryManifest)
add_dependencies(PrismatiXPreviewHost PrismatiXUiTypeRegistryManifest)
# The built-in screens as .pxui documents, for `prismatix eject ui`.
set(PRISMATIX_BUILTIN_UI_DIR "${CMAKE_CURRENT_BINARY_DIR}/BuiltinUI")
add_custom_command(
    OUTPUT "${PRISMATIX_BUILTIN_UI_DIR}/title.pxui"
    COMMAND $<TARGET_FILE:PrismatiXUiTypeManifest> --builtin-ui "${PRISMATIX_BUILTIN_UI_DIR}"
    DEPENDS PrismatiXUiTypeManifest
    COMMENT "Exporting the built-in UI screens"
    VERBATIM
)
add_custom_target(PrismatiXBuiltinUiScreens ALL
                  DEPENDS "${PRISMATIX_BUILTIN_UI_DIR}/title.pxui")
add_dependencies(PrismatiXPreviewHost PrismatiXBuiltinUiScreens)
install(DIRECTORY "${PRISMATIX_BUILTIN_UI_DIR}/"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/PrismatiX/BuiltinUI
    COMPONENT PrismatiXRuntime
)
add_executable(PrismatiXInputKeyManifest Applications/InputKeyManifestMain.cpp)
target_link_libraries(PrismatiXInputKeyManifest PRIVATE PrismatiXRuntime)
prismatix_configure_target(PrismatiXInputKeyManifest)
set(PRISMATIX_INPUT_KEY_MANIFEST
    "${CMAKE_CURRENT_BINARY_DIR}/PrismatiXInputKeyRegistry.json")
add_custom_command(
    OUTPUT "${PRISMATIX_INPUT_KEY_MANIFEST}"
    COMMAND $<TARGET_FILE:PrismatiXInputKeyManifest>
            "${PRISMATIX_INPUT_KEY_MANIFEST}"
    DEPENDS PrismatiXInputKeyManifest
    COMMENT "Exporting the native input key-name contract"
    VERBATIM
)
add_custom_target(PrismatiXInputKeyRegistryManifest ALL
                  DEPENDS "${PRISMATIX_INPUT_KEY_MANIFEST}")
add_dependencies(PrismatiXPackager PrismatiXInputKeyRegistryManifest)
add_dependencies(PrismatiXPlayer PrismatiXInputKeyRegistryManifest)
add_dependencies(PrismatiXPlayerQA PrismatiXInputKeyRegistryManifest)
add_dependencies(PrismatiXPreviewHost PrismatiXInputKeyRegistryManifest)
install(FILES "${PRISMATIX_INPUT_KEY_MANIFEST}"
    DESTINATION .
    COMPONENT PrismatiXSDK
)
install(FILES "${PRISMATIX_INPUT_KEY_MANIFEST}"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/PrismatiX
    COMPONENT PrismatiXRuntime
)
install(FILES "${PRISMATIX_UI_TYPE_MANIFEST}"
    DESTINATION .
    COMPONENT PrismatiXSDK
)
install(FILES "${PRISMATIX_UI_TYPE_MANIFEST}"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/PrismatiX
    COMPONENT PrismatiXRuntime
)
add_executable(PrismatiXSDKReference Applications/SDKReferenceMain.cpp)
target_link_libraries(PrismatiXSDKReference PRIVATE PrismatiXSDK)
prismatix_configure_target(PrismatiXSDKReference)
set(PRISMATIX_SDK_REFERENCE
    "${CMAKE_CURRENT_BINARY_DIR}/PrismatiXSDKReference.md")
add_custom_command(
    OUTPUT "${PRISMATIX_SDK_REFERENCE}"
    COMMAND $<TARGET_FILE:PrismatiXSDKReference>
            "${CMAKE_CURRENT_SOURCE_DIR}/../Contracts/contract-manifest.json"
            "${PRISMATIX_UI_TYPE_MANIFEST}"
            "${PRISMATIX_SDK_REFERENCE}"
    DEPENDS PrismatiXSDKReference
            "${CMAKE_CURRENT_SOURCE_DIR}/../Contracts/contract-manifest.json"
            "${PRISMATIX_UI_TYPE_MANIFEST}"
    COMMENT "Generating the SDK reference from contracts and UI TypeRegistry"
    VERBATIM
)
add_custom_target(PrismatiXSDKReferenceDocument ALL
                  DEPENDS "${PRISMATIX_SDK_REFERENCE}")
install(FILES "${PRISMATIX_SDK_REFERENCE}"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/PrismatiX/docs
    COMPONENT PrismatiXSDK
)
if(UNIX)
    if(APPLE)
        set(_PRISMATIX_RUNTIME_RPATH "@loader_path")
    else()
        set(_PRISMATIX_RUNTIME_RPATH "$ORIGIN")
    endif()
    set_target_properties(PrismatiXPackager PrismatiXPreviewHost PrismatiXPlayer
        PROPERTIES INSTALL_RPATH "${_PRISMATIX_RUNTIME_RPATH}")
    # A packed game keeps its libraries in <Name>.app/Contents/Frameworks
    # (macOS) or lib/ (Linux); the installed runtime keeps them beside the
    # Player. DT_RPATH rather than DT_RUNPATH, so libraries in lib/ also find
    # each other through the executable's search path.
    if(APPLE)
        set_property(TARGET PrismatiXPlayer APPEND PROPERTY INSTALL_RPATH
            "@executable_path/../Frameworks")
    else()
        set_property(TARGET PrismatiXPlayer APPEND PROPERTY INSTALL_RPATH "$ORIGIN/lib")
        target_link_options(PrismatiXPlayer PRIVATE "LINKER:--disable-new-dtags")
    endif()
    install(TARGETS PrismatiXPackager PrismatiXPreviewHost PrismatiXPlayer
        RUNTIME_DEPENDENCY_SET PrismatiXRuntimeDependencies
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT PrismatiXRuntime
    )
    set(_PRISMATIX_RUNTIME_DEPENDENCY_DIRECTORIES)
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        list(APPEND _PRISMATIX_RUNTIME_DEPENDENCY_DIRECTORIES
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
    endif()
    install(RUNTIME_DEPENDENCY_SET PrismatiXRuntimeDependencies
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT PrismatiXRuntime
        DIRECTORIES ${_PRISMATIX_RUNTIME_DEPENDENCY_DIRECTORIES}
        PRE_EXCLUDE_REGEXES
            "^api-ms-" "^ext-ms-"
        POST_EXCLUDE_REGEXES
            "^/lib/" "^/lib64/" "^/usr/lib/" "^/System/Library/"
    )
else()
    install(TARGETS PrismatiXPackager PrismatiXPreviewHost PrismatiXPlayer
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT PrismatiXRuntime
    )
endif()
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/install/runtime-manifest.json"
    "${CMAKE_CURRENT_BINARY_DIR}/release-inventory.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE"
    "${CMAKE_CURRENT_SOURCE_DIR}/../THIRD_PARTY_NOTICES.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/../ASSET_PROVENANCE.json"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/PrismatiX
    COMPONENT PrismatiXRuntime
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Fonts/OFL-1.1.txt"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/PrismatiX/licenses/fonts
    COMPONENT PrismatiXRuntime
)
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/../docs/releases/delivery-contract.md"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/PrismatiX/docs/releases
    COMPONENT PrismatiXRuntime
)
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    # Windows redistributes the selected triplet's DLL set below. Collect that
    # same installation's actual notices, including transitive dependencies;
    # a hand-maintained OPTIONAL list silently omitted newly added libraries.
    file(GLOB _PRISMATIX_DEPENDENCY_NOTICES CONFIGURE_DEPENDS
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/*/copyright")
    foreach(_PRISMATIX_NOTICE IN LISTS _PRISMATIX_DEPENDENCY_NOTICES)
        get_filename_component(_PRISMATIX_NOTICE_DIRECTORY "${_PRISMATIX_NOTICE}" DIRECTORY)
        get_filename_component(_PRISMATIX_NOTICE_PACKAGE "${_PRISMATIX_NOTICE_DIRECTORY}" NAME)
        install(FILES
            "${_PRISMATIX_NOTICE}"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/PrismatiX/licenses/${_PRISMATIX_NOTICE_PACKAGE}"
            COMPONENT PrismatiXRuntime
        )
    endforeach()
endif()
# QuickJS-NG is fetched from the pinned source revision rather than vcpkg.
file(GLOB _PRISMATIX_QUICKJS_NOTICES CONFIGURE_DEPENDS
    "${quickjs_ng_SOURCE_DIR}/LICENSE*" "${quickjs_ng_SOURCE_DIR}/COPYING*")
if(NOT _PRISMATIX_QUICKJS_NOTICES)
    message(FATAL_ERROR "Pinned QuickJS-NG source has no LICENSE or COPYING notice")
endif()
install(FILES ${_PRISMATIX_QUICKJS_NOTICES}
    DESTINATION "${CMAKE_INSTALL_DATADIR}/PrismatiX/licenses/quickjs-ng"
    COMPONENT PrismatiXRuntime)
if(WIN32 AND DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    install(DIRECTORY
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin/"
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        CONFIGURATIONS Debug
        COMPONENT PrismatiXRuntime
        FILES_MATCHING PATTERN "*.dll"
    )
    install(DIRECTORY
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/"
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        CONFIGURATIONS Release RelWithDebInfo MinSizeRel
        COMPONENT PrismatiXRuntime
        FILES_MATCHING PATTERN "*.dll"
    )
endif()
endif()  # NOT ANDROID
endif()

if(PRISMATIX_BUILD_WASM_PREVIEW)
    add_executable(PrismatiXPreviewWasm
        Applications/PreviewWasmMain.cpp
        Engine/Runtime.cpp
        Engine/Platform/AppLifecycle.cpp
        Engine/Platform/Input.cpp
        Engine/Platform/TouchGestures.cpp
        Engine/Platform/Window.cpp
        Engine/IO/FileSource.cpp
        Engine/IO/VFS.cpp
    )
    prismatix_configure_target(PrismatiXPreviewWasm)
    target_compile_definitions(PrismatiXPreviewWasm PRIVATE
        PRISMATIX_PREVIEW_WASM=1)
    target_link_libraries(PrismatiXPreviewWasm PRIVATE
        PrismatiXRuntimeCore)
    set_target_properties(PrismatiXPreviewWasm PROPERTIES
        OUTPUT_NAME "prismatix-preview"
        SUFFIX ".mjs"
    )
    target_link_options(PrismatiXPreviewWasm PRIVATE
        "-sMODULARIZE=1"
        "-sEXPORT_ES6=1"
        "-sEXPORT_NAME=createPrismatiXPreview"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sENVIRONMENT=web,worker,node"
        "-sFILESYSTEM=1"
        "-sNO_EXIT_RUNTIME=0"
        "$<$<CONFIG:Release>:-O3>"
        "$<$<CONFIG:Release>:-gsource-map>"
        "-sEXPORTED_RUNTIME_METHODS=['cwrap','UTF8ToString','FS']"
        "-sEXPORTED_FUNCTIONS=['_malloc','_free','_px_preview_create','_px_preview_apply','_px_preview_patch','_px_preview_control','_px_preview_resize','_px_preview_tick','_px_preview_drain_events','_px_preview_test_conformance','_px_preview_destroy']"
        "SHELL:--embed-file \"${PROJECT_SOURCE_DIR}/Resources/Fonts/NotoSansTC-Bold.ttf@/project/Resources/Fonts/NotoSansTC-Bold.ttf\""
        "--source-map-base" "./"
    )
    add_custom_command(TARGET PrismatiXPreviewWasm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_BINARY_DIR}/preview-contract.json"
            "$<TARGET_FILE_DIR:PrismatiXPreviewWasm>/preview-contract.json"
        VERBATIM)
endif()

if(PRISMATIX_BUILD_WASM_PLAYER)
    option(PRISMATIX_WEB_DIAGNOSTICS "Keep Web Player function names and runtime assertions" OFF)
    target_compile_definitions(PrismatiXRuntimeCore PRIVATE PRISMATIX_WEB_PLAYER=1)
    if(PRISMATIX_WEB_RENDERER STREQUAL "webgpu")
        # The WebGPU renderer runs the desktop Compositor2D path on SDL_GPU. Its
        # built-in compositor and rule shaders are the Naga WGSL of the native
        # SPIR-V, rebound for SDL_GPU's WebGPU backend. They are generated and
        # checked by scripts/build-wasm-player.mjs.
        set(PRISMATIX_WEBGPU_SHADER_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/BuiltInWebGpuShaders.generated.cpp")
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Builtin/webgpu/compositor.frag.wgsl" PRISMATIX_WGSL_COMPOSITOR)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Builtin/webgpu/rule.frag.wgsl" PRISMATIX_WGSL_RULE)
        file(CONFIGURE OUTPUT "${PRISMATIX_WEBGPU_SHADER_CPP}" CONTENT [=[
namespace px::graphics {
extern const char kBuiltInCompositorWgsl[] = R"PXWGSL(@PRISMATIX_WGSL_COMPOSITOR@)PXWGSL";
extern const char kBuiltInRuleWgsl[] = R"PXWGSL(@PRISMATIX_WGSL_RULE@)PXWGSL";
}
]=] @ONLY)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Builtin/webgpu/compositor.frag.wgsl"
            "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Builtin/webgpu/rule.frag.wgsl")
        target_sources(PrismatiXRuntimeCore PRIVATE "${PRISMATIX_WEBGPU_SHADER_CPP}")
        target_compile_definitions(PrismatiXRuntimeCore PRIVATE PRISMATIX_WEB_WEBGPU=1)
    endif()
    file(GLOB_RECURSE PRISMATIX_WEB_PLATFORM_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Engine/IO/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Platform/*.cpp")
    add_library(PrismatiXRuntimePlatformWeb STATIC
        ${PRISMATIX_WEB_PLATFORM_SOURCES}
        Engine/Runtime.cpp
        Engine/Accessibility/SpeechService.cpp
        Engine/Accessibility/PlatformSemanticAdapter.cpp
        Engine/Resources/AssetRegistry.cpp
        Engine/UI/UITypeManifest.cpp
        Engine/Progression/GameSettings.cpp
        Engine/Progression/GlobalProfileStore.cpp
        Engine/Progression/Persist.cpp
        Engine/Progression/SaveSystem.cpp)
    target_compile_definitions(PrismatiXRuntimePlatformWeb PRIVATE PRISMATIX_WEB_PLAYER=1)
    target_include_directories(PrismatiXRuntimePlatformWeb PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/Engine)
    target_link_libraries(PrismatiXRuntimePlatformWeb PUBLIC
        PrismatiXRuntimeCore prismatix_mbedcrypto zstd::libzstd)
    target_link_libraries(PrismatiXRuntimeCore PRIVATE PrismatiXRuntimePlatformWeb)
    prismatix_configure_target(PrismatiXRuntimePlatformWeb)
    add_executable(PrismatiXPlayerWeb
        Applications/PlayerWasmMain.cpp
        Applications/Player/PlayerApp.cpp
        Applications/Player/PlayerActions.cpp
        Applications/Player/PlayerSettings.cpp
        Applications/Player/QaIpc.cpp)
    target_compile_definitions(PrismatiXPlayerWeb PRIVATE PRISMATIX_WEB_PLAYER=1 PRISMATIX_QA_HOOKS=0)
    if(PRISMATIX_WEB_RENDERER STREQUAL "webgpu")
        foreach(web_target PrismatiXRuntimePlatformWeb PrismatiXPlayerWeb)
            target_compile_definitions(${web_target} PRIVATE PRISMATIX_WEB_WEBGPU=1)
        endforeach()
    endif()
    target_link_libraries(PrismatiXPlayerWeb PRIVATE PrismatiXRuntimePlatformWeb)
    prismatix_configure_target(PrismatiXPlayerWeb)
    # The shipping Player catches malformed resources and persistence errors.
    # Emscripten otherwise strips C++ catch paths and aborts on those inputs.
    foreach(web_target PrismatiXRuntimeCore PrismatiXRuntimePlatformWeb PrismatiXPlayerWeb)
        target_compile_options(${web_target} PRIVATE -fexceptions)
    endforeach()
    set_target_properties(PrismatiXPlayerWeb PROPERTIES OUTPUT_NAME "prismatix-player" SUFFIX ".mjs")
    target_link_options(PrismatiXPlayerWeb PRIVATE
        -fexceptions
        "-sMODULARIZE=1" "-sEXPORT_ES6=1" "-sEXPORT_NAME=createPrismatiXPlayer"
        "-sALLOW_MEMORY_GROWTH=1" "-sMAXIMUM_MEMORY=2147483648"
        "-sSTACK_SIZE=1048576"
        "-sENVIRONMENT=web" "-sFILESYSTEM=1" "-sASYNCIFY=1"
        "-sASYNCIFY_STACK_SIZE=262144" "-sFORCE_FILESYSTEM=1"
        "-sMIN_WEBGL_VERSION=2" "-sMAX_WEBGL_VERSION=2"
        "-sEXIT_RUNTIME=0" "-lidbfs.js"
        "-sEXPORTED_RUNTIME_METHODS=['FS','ENV','callMain','stringToNewUTF8','requestFullscreen']"
        "-sEXPORTED_FUNCTIONS=['_main','_malloc','_free']")
    if(PRISMATIX_WEB_DIAGNOSTICS)
        target_link_options(PrismatiXPlayerWeb PRIVATE "--profiling-funcs" "-sASSERTIONS=1")
    endif()
    # Both renderers suspend through Asyncify: frame waits, remote reads and,
    # on WebGPU, the backend's adapter/device requests, fences and swapchain
    # acquires. JSPI would need wasm exceptions and wasm setjmp/longjmp in
    # every dependency and is missing from Safari (docs/web-export.md).
    message(STATUS "PrismatiX Web Player: renderer=${PRISMATIX_WEB_RENDERER} SDL=${PRISMATIX_WEB_SDL_COMMIT}")
endif()

if(BUILD_TESTING AND NOT EMSCRIPTEN AND NOT ANDROID)
    # Contract and integration tests use assert() as their executable test
    # primitive. Keep those checks active in Release/RelWithDebInfo quality
    # gates; otherwise NDEBUG removes both validation and assertion operands.
    if(MSVC)
        add_compile_options(/UNDEBUG)
    else()
        add_compile_options(-UNDEBUG)
    endif()

    add_executable(PrismatiXPreviewProtocolTests
                   Tests/PreviewProtocolTests.cpp)
    target_link_libraries(PrismatiXPreviewProtocolTests PRIVATE
                          PrismatiXRuntimeCore)
    prismatix_configure_target(PrismatiXPreviewProtocolTests)
    add_test(NAME PrismatiX.Preview.WasmProtocol
             COMMAND PrismatiXPreviewProtocolTests)
    set_tests_properties(PrismatiX.Preview.WasmProtocol
                         PROPERTIES LABELS "preview;wasm;contract")

    add_executable(PrismatiXPreviewSessionContractTests
                   Tests/PreviewSessionContractTests.cpp)
    target_link_libraries(PrismatiXPreviewSessionContractTests PRIVATE
                          PrismatiXRuntime)
    prismatix_configure_target(PrismatiXPreviewSessionContractTests)
    add_test(NAME PrismatiX.SDK.PreviewSessionContract
             COMMAND PrismatiXPreviewSessionContractTests)
    set_tests_properties(PrismatiX.SDK.PreviewSessionContract PROPERTIES
                         LABELS "sdk;preview;runtime;contract;checkpoint;seek")

    # Compile the real WASM entrypoint with the native toolchain as a fast
    # syntax/link gate when Emscripten is not installed on the workstation.
    # The Emscripten target remains the authoritative binary gate.
    add_executable(PrismatiXPreviewWasmNativeCompileTests
                   Applications/PreviewWasmMain.cpp)
    target_link_libraries(PrismatiXPreviewWasmNativeCompileTests
                          PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXPreviewWasmNativeCompileTests)
    add_test(NAME PrismatiX.Preview.WasmEntrypointNativeCompile
             COMMAND PrismatiXPreviewWasmNativeCompileTests)
    set_tests_properties(PrismatiX.Preview.WasmEntrypointNativeCompile
                         PROPERTIES LABELS "preview;wasm;compile")

    add_executable(PrismatiXPerformancePreviewContractTests
                   Tests/PerformancePreviewContractTests.cpp)
    target_link_libraries(PrismatiXPerformancePreviewContractTests PRIVATE
                          PrismatiXRuntimeCore)
    prismatix_configure_target(PrismatiXPerformancePreviewContractTests)
    add_test(NAME PrismatiX.Preview.PerformanceContract
             COMMAND PrismatiXPerformancePreviewContractTests)
    set_tests_properties(PrismatiX.Preview.PerformanceContract
                         PROPERTIES LABELS "preview;wasm;performance;contract")

    add_executable(PrismatiXLocalizationPreviewContractTests
                   Tests/LocalizationPreviewContractTests.cpp)
    target_link_libraries(PrismatiXLocalizationPreviewContractTests PRIVATE
                          PrismatiXRuntimeCore)
    prismatix_configure_target(PrismatiXLocalizationPreviewContractTests)
    add_test(NAME PrismatiX.Preview.LocalizationContract
             COMMAND PrismatiXLocalizationPreviewContractTests)
    set_tests_properties(PrismatiX.Preview.LocalizationContract
                         PROPERTIES LABELS "preview;wasm;localization;contract")

    add_executable(PrismatiXProgressionPreviewContractTests
                   Tests/ProgressionPreviewContractTests.cpp)
    target_link_libraries(PrismatiXProgressionPreviewContractTests PRIVATE
                          PrismatiXRuntimeCore)
    prismatix_configure_target(PrismatiXProgressionPreviewContractTests)
    add_test(NAME PrismatiX.Preview.ProgressionContract
             COMMAND PrismatiXProgressionPreviewContractTests)
    set_tests_properties(PrismatiX.Preview.ProgressionContract
                         PROPERTIES LABELS "preview;wasm;progression;contract")

    add_executable(PrismatiXPackagerContractTests Tests/PackagerContractTests.cpp)
    target_link_libraries(PrismatiXPackagerContractTests PRIVATE
                          PrismatiXRuntime PrismatiXPackagerSDK)
    if(WIN32)
        target_link_libraries(PrismatiXPackagerContractTests PRIVATE psapi)
    endif()
    target_compile_definitions(PrismatiXPackagerContractTests PRIVATE
        PRISMATIX_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../Resources")
    prismatix_configure_target(PrismatiXPackagerContractTests)
    add_test(NAME PrismatiX.SDK.PackagerContract
             COMMAND PrismatiXPackagerContractTests)
    set_tests_properties(PrismatiX.SDK.PackagerContract
                         PROPERTIES LABELS "sdk;contract;packager;gpu;native")

    # Icons, version information and the per-platform distribution layouts.
    add_executable(PrismatiXPackageBrandingTests Tests/PackageBrandingTests.cpp)
    target_link_libraries(PrismatiXPackageBrandingTests PRIVATE PrismatiXPackagerSDK)
    if(WIN32)
        target_link_libraries(PrismatiXPackageBrandingTests PRIVATE version)
    endif()
    prismatix_configure_target(PrismatiXPackageBrandingTests)
    add_test(NAME PrismatiX.SDK.PackageBranding
             COMMAND PrismatiXPackageBrandingTests
                     $<TARGET_FILE:PrismatiXPlayer> $<TARGET_FILE:PrismatiXPlayerQA>)
    set_tests_properties(PrismatiX.SDK.PackageBranding
                         PROPERTIES LABELS "sdk;contract;packager")

    # Invoked by scripts/release-journey.mjs after public CLI packaging
    # (PrismatiX.SDK.ReleaseJourney below). It has no CTest entry of its own:
    # the verifier never synthesizes Runtime IR or bypasses the authoring path.
    add_executable(PrismatiXReleaseJourneyTests Tests/ReleaseJourneyTests.cpp)
    target_link_libraries(PrismatiXReleaseJourneyTests PRIVATE
                          PrismatiXRuntime PrismatiXPackagerSDK)
    prismatix_configure_target(PrismatiXReleaseJourneyTests)

    add_executable(PrismatiXCharacterResourcesContractTests
                   Tests/CharacterResourcesContractTests.cpp)
    target_link_libraries(PrismatiXCharacterResourcesContractTests
                          PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXCharacterResourcesContractTests)
    add_test(NAME PrismatiX.SDK.CharacterResourcesContract
             COMMAND PrismatiXCharacterResourcesContractTests)
    set_tests_properties(PrismatiX.SDK.CharacterResourcesContract
                         PROPERTIES LABELS "sdk;runtime;contract;characters")

    add_executable(PrismatiXGameCatalogResourcesContractTests
                   Tests/GameCatalogResourcesContractTests.cpp)
    target_link_libraries(PrismatiXGameCatalogResourcesContractTests
                          PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXGameCatalogResourcesContractTests)
    add_test(NAME PrismatiX.SDK.GameCatalogResourcesContract
             COMMAND PrismatiXGameCatalogResourcesContractTests)
    set_tests_properties(PrismatiX.SDK.GameCatalogResourcesContract
                         PROPERTIES LABELS "sdk;runtime;contract;catalog")

    add_executable(PrismatiXPlayerCatalogNativeAcceptanceTests
                   Tests/PlayerCatalogNativeAcceptanceTests.cpp)
    target_link_libraries(PrismatiXPlayerCatalogNativeAcceptanceTests
                          PRIVATE PrismatiXRuntime PrismatiXPackagerSDK)
    if(WIN32)
        target_link_libraries(PrismatiXPlayerCatalogNativeAcceptanceTests
                              PRIVATE user32 gdi32)
    endif()
    prismatix_configure_target(PrismatiXPlayerCatalogNativeAcceptanceTests)
    add_dependencies(PrismatiXPlayerCatalogNativeAcceptanceTests
                     PrismatiXPlayerQA)
    # The journey itself is Tests/journeys/catalog.mjs, run by `prismatix
    # test` (packages/cli, built by `pnpm run build`) over the QA IPC.
    find_program(PRISMATIX_NODE_EXECUTABLE node)
    if(PRISMATIX_NODE_EXECUTABLE)
        add_test(NAME PrismatiX.SDK.PackageVerifierColdStart
            COMMAND "${PRISMATIX_NODE_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-package-verifier-cold-start.mjs"
                    $<TARGET_FILE:PrismatiXPackager>)
        # Each of its 33 fresh Packager processes verifies with four workers;
        # PROCESSORS keeps a parallel ctest from oversubscribing the machine.
        set_tests_properties(PrismatiX.SDK.PackageVerifierColdStart PROPERTIES
            LABELS "sdk;contract;packager;native" TIMEOUT 90 PROCESSORS 4)
        # V1 and V2 of Tests/Fixtures/ReleaseJourney through `prismatix
        # validate`, `build` and `pack`, then V1 saves upgraded by the V2
        # package's migration (scripts/release-journey.mjs).
        add_test(NAME PrismatiX.SDK.ReleaseJourney
            COMMAND "${PRISMATIX_NODE_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/release-journey.mjs"
                    --cli "${CMAKE_CURRENT_SOURCE_DIR}/../packages/cli/dist/src/cli.js"
                    --packager $<TARGET_FILE:PrismatiXPackager>
                    --player $<TARGET_FILE:PrismatiXPlayer>
                    --verifier $<TARGET_FILE:PrismatiXReleaseJourneyTests>)
        set_tests_properties(PrismatiX.SDK.ReleaseJourney PROPERTIES
            LABELS "sdk;e2e;packager;release;native" TIMEOUT 300 PROCESSORS 2)
        # A throwing engine.ready handler, a missing start-route template and
        # a damaged or missing Content.pdx: the packaged Player reports each
        # (PXPLAYER5031) and exits with 1 instead of crashing, and `prismatix
        # test` names the cause (scripts/test-player-boot-failure.mjs).
        add_test(NAME PrismatiX.Player.BootFailure
            COMMAND "${PRISMATIX_NODE_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/test-player-boot-failure.mjs"
                    --cli "${CMAKE_CURRENT_SOURCE_DIR}/../packages/cli/dist/src/cli.js"
                    --packager $<TARGET_FILE:PrismatiXPackager>
                    --player $<TARGET_FILE:PrismatiXPlayer>
                    --player-qa $<TARGET_FILE:PrismatiXPlayerQA>)
        set_tests_properties(PrismatiX.Player.BootFailure PROPERTIES
            LABELS "native;e2e;player" TIMEOUT 300 PROCESSORS 2)
    endif()
    add_test(
        NAME PrismatiX.Player.CatalogNativeAcceptance
        COMMAND PrismatiXPlayerCatalogNativeAcceptanceTests
                $<TARGET_FILE:PrismatiXPlayerQA>
                ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/MultiLocaleVN/Content
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Fonts/NotoSansTC-Bold.ttf
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Branding/PrismatiXEngine_Logo.png
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Branding/logo.wav
                "${PRISMATIX_NODE_EXECUTABLE}"
                ${CMAKE_CURRENT_SOURCE_DIR}/../packages/cli/dist/src/cli.js
                ${CMAKE_CURRENT_SOURCE_DIR}/Tests/journeys/catalog.mjs)
    if(NOT PRISMATIX_NODE_EXECUTABLE)
        message(STATUS "Node.js not found; PrismatiX.Player.CatalogNativeAcceptance is disabled")
        set_tests_properties(PrismatiX.Player.CatalogNativeAcceptance
                             PROPERTIES DISABLED TRUE)
    endif()
    # The shipped Player must not even contain the QA hook names.
    add_test(
        NAME PrismatiX.Player.ShippedWithoutQaHooks
        COMMAND ${CMAKE_COMMAND}
                -DSHIPPED=$<TARGET_FILE:PrismatiXPlayer>
                -DQA=$<TARGET_FILE:PrismatiXPlayerQA>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/PlayerQaHooksGate.cmake)
    set_tests_properties(PrismatiX.Player.ShippedWithoutQaHooks
                         PROPERTIES LABELS "native;player;release")
    set_tests_properties(
        PrismatiX.Player.CatalogNativeAcceptance
        PROPERTIES
            LABELS "native;e2e;player;characters"
            RUN_SERIAL TRUE
            TIMEOUT 120)

    add_test(NAME PrismatiX.SDK.PackagerCliContract
        COMMAND ${CMAKE_COMMAND}
            -DPACKAGER=$<TARGET_FILE:PrismatiXPackager>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/PackagerCliContract.cmake)
    set_tests_properties(PrismatiX.SDK.PackagerCliContract
                         PROPERTIES LABELS "sdk;contract;packager")

    add_executable(PrismatiXUiContractTests Tests/UiContractTests.cpp)
    target_link_libraries(PrismatiXUiContractTests PRIVATE PrismatiXSDK)
    prismatix_configure_target(PrismatiXUiContractTests)
    add_test(NAME PrismatiX.SDK.UiContract COMMAND PrismatiXUiContractTests)
    set_tests_properties(PrismatiX.SDK.UiContract PROPERTIES LABELS "sdk;contract;ui")

    add_executable(PrismatiXUiRuntimeAdapterTests Tests/UiRuntimeAdapterTests.cpp)
    target_link_libraries(PrismatiXUiRuntimeAdapterTests PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXUiRuntimeAdapterTests)
    add_test(NAME PrismatiX.SDK.UiRuntimeAdapter COMMAND PrismatiXUiRuntimeAdapterTests)
    set_tests_properties(PrismatiX.SDK.UiRuntimeAdapter PROPERTIES LABELS "sdk;runtime;contract;ui")

    add_executable(PrismatiXVirtualizedViewTests Tests/VirtualizedViewTests.cpp)
    target_link_libraries(PrismatiXVirtualizedViewTests PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXVirtualizedViewTests)
    add_test(NAME PrismatiX.SDK.VirtualizedView COMMAND PrismatiXVirtualizedViewTests)
    set_tests_properties(PrismatiX.SDK.VirtualizedView PROPERTIES LABELS "sdk;runtime;ui")

    add_executable(PrismatiXUiTypeRegistryContractTests
                   Tests/UiTypeRegistryContractTests.cpp)
    target_link_libraries(PrismatiXUiTypeRegistryContractTests
                          PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXUiTypeRegistryContractTests)
    add_test(NAME PrismatiX.SDK.UiTypeRegistryContract
             COMMAND PrismatiXUiTypeRegistryContractTests)
    set_tests_properties(PrismatiX.SDK.UiTypeRegistryContract
                         PROPERTIES LABELS "sdk;runtime;contract;ui")

    add_test(NAME PrismatiX.SDK.ReferenceContract
             COMMAND PrismatiXSDKReference --check
                     "${CMAKE_CURRENT_SOURCE_DIR}/../Contracts/contract-manifest.json"
                     "${PRISMATIX_UI_TYPE_MANIFEST}"
                     "${PRISMATIX_SDK_REFERENCE}")
    set_tests_properties(PrismatiX.SDK.ReferenceContract
                         PROPERTIES LABELS "sdk;contract;documentation")

    add_executable(PrismatiXSDKManifestContractTests
                   Tests/SDKManifestContractTests.cpp)
    target_link_libraries(PrismatiXSDKManifestContractTests
                          PRIVATE PrismatiXRuntime)
    target_compile_definitions(PrismatiXSDKManifestContractTests PRIVATE
        "PRISMATIX_SDK_MANIFEST_PATH=\"${CMAKE_CURRENT_BINARY_DIR}/PrismatiXSDKManifest.json\""
        "PRISMATIX_RUNTIME_MANIFEST_PATH=\"${CMAKE_CURRENT_BINARY_DIR}/install/runtime-manifest.json\""
        "PRISMATIX_EXECUTABLE_SUFFIX=\"${CMAKE_EXECUTABLE_SUFFIX}\"")
    prismatix_configure_target(PrismatiXSDKManifestContractTests)
    add_test(NAME PrismatiX.SDK.ManifestContract
             COMMAND PrismatiXSDKManifestContractTests)
    set_tests_properties(PrismatiX.SDK.ManifestContract PROPERTIES
                         LABELS "sdk;contract;architecture")

    add_executable(PrismatiXRuntimeIrContractTests Tests/RuntimeIrContractTests.cpp)
    target_link_libraries(PrismatiXRuntimeIrContractTests PRIVATE PrismatiXSDK)
    target_compile_definitions(PrismatiXRuntimeIrContractTests PRIVATE
        "PRISMATIX_STORY_GOLDEN_IR_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/../Contracts/fixtures/story-compiler-golden.pxir\"")
    prismatix_configure_target(PrismatiXRuntimeIrContractTests)
    add_test(NAME PrismatiX.SDK.RuntimeIrContract COMMAND PrismatiXRuntimeIrContractTests)
    set_tests_properties(PrismatiX.SDK.RuntimeIrContract PROPERTIES LABELS "sdk;contract")

    add_executable(PrismatiXRuntimeIrRuntimeAdapterTests
        Tests/RuntimeIrRuntimeAdapterTests.cpp)
    target_link_libraries(PrismatiXRuntimeIrRuntimeAdapterTests PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXRuntimeIrRuntimeAdapterTests)
    target_compile_definitions(PrismatiXRuntimeIrRuntimeAdapterTests PRIVATE
        PRISMATIX_RUNTIME_IR_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../Contracts/runtime-ir.schema.json")
    add_test(NAME PrismatiX.SDK.RuntimeIrRuntimeAdapter
             COMMAND PrismatiXRuntimeIrRuntimeAdapterTests)
    set_tests_properties(PrismatiX.SDK.RuntimeIrRuntimeAdapter
                         PROPERTIES LABELS "sdk;runtime;contract")

    add_executable(PrismatiXRuntimeIrPatchTests
                   Tests/RuntimeIrPatchTests.cpp)
    target_link_libraries(PrismatiXRuntimeIrPatchTests PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXRuntimeIrPatchTests)
    add_test(NAME PrismatiX.Runtime.RuntimeIrPatch
             COMMAND PrismatiXRuntimeIrPatchTests)
    set_tests_properties(PrismatiX.Runtime.RuntimeIrPatch
                         PROPERTIES LABELS "runtime;contract;preview;patch")

    add_executable(PrismatiXStorySyntaxRuntimeTests
                   Tests/StorySyntaxRuntimeTests.cpp)
    target_link_libraries(PrismatiXStorySyntaxRuntimeTests PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXStorySyntaxRuntimeTests)
    add_test(NAME PrismatiX.Runtime.StorySyntax
             COMMAND PrismatiXStorySyntaxRuntimeTests)
    set_tests_properties(PrismatiX.Runtime.StorySyntax
                         PROPERTIES LABELS "runtime;contract;narrative")

    add_executable(PrismatiXRuntimeAssetResolverTests
                   Tests/RuntimeAssetResolverTests.cpp)
    target_link_libraries(PrismatiXRuntimeAssetResolverTests
                          PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXRuntimeAssetResolverTests)
    add_test(NAME PrismatiX.Runtime.AssetResolver
             COMMAND PrismatiXRuntimeAssetResolverTests)
    set_tests_properties(PrismatiX.Runtime.AssetResolver
                         PROPERTIES LABELS "runtime;contract;narrative;resource")

    add_test(NAME PrismatiX.SDK.PreviewHostProtocol
        COMMAND ${CMAKE_COMMAND}
            -DPREVIEW_HOST=$<TARGET_FILE:PrismatiXPreviewHost>
            -DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/Tests/PreviewHostInput.ndjson
            -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/PreviewHostSmoke.cmake)
    set_tests_properties(PrismatiX.SDK.PreviewHostProtocol
                         PROPERTIES LABELS "sdk;contract;preview")
    add_test(NAME PrismatiX.SDK.PreviewHostScriptDebug
        COMMAND ${CMAKE_COMMAND}
            -DPREVIEW_HOST=$<TARGET_FILE:PrismatiXPreviewHost>
            -DFIXTURE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/PreviewHostScript
            -DINPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/PreviewHostScriptDebugInput.ndjson
            -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/PreviewHostScriptDebug.cmake)
    set_tests_properties(PrismatiX.SDK.PreviewHostScriptDebug
                         PROPERTIES LABELS "sdk;contract;preview;script;javascript;debugger")
    add_test(NAME PrismatiX.SDK.PreviewHostScriptAction
        COMMAND ${CMAKE_COMMAND}
            -DPREVIEW_HOST=$<TARGET_FILE:PrismatiXPreviewHost>
            -DFIXTURE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/PreviewHostScript
            -DINPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/PreviewHostScriptActionInput.ndjson
            -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/PreviewHostScriptAction.cmake)
    set_tests_properties(PrismatiX.SDK.PreviewHostScriptAction
                         PROPERTIES LABELS "sdk;contract;preview;script;javascript;action")
    add_test(NAME PrismatiX.SDK.PreviewHostRuntimeAssets
        COMMAND ${CMAKE_COMMAND}
            -DPREVIEW_HOST=$<TARGET_FILE:PrismatiXPreviewHost>
            -DFIXTURE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/PreviewHostScript
            -DINPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/PreviewHostRuntimeAssetsInput.ndjson
            -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/PreviewHostRuntimeAssets.cmake)
    set_tests_properties(PrismatiX.SDK.PreviewHostRuntimeAssets
                         PROPERTIES LABELS "sdk;contract;preview;narrative;resource")
    add_test(NAME PrismatiX.SDK.PreviewHostResolution
        COMMAND ${CMAKE_COMMAND}
            -DPREVIEW_HOST=$<TARGET_FILE:PrismatiXPreviewHost>
            -DFIXTURE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/PreviewHostScript
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/PreviewHostResolution
            -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/PreviewHostResolution.cmake)
    set_tests_properties(PrismatiX.SDK.PreviewHostResolution
                         PROPERTIES LABELS "sdk;contract;preview")
    add_test(NAME PrismatiX.SDK.PreviewHostStorySeek
        COMMAND ${CMAKE_COMMAND}
            -DPREVIEW_HOST=$<TARGET_FILE:PrismatiXPreviewHost>
            -DFIXTURE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/PreviewHostScript
            -DINPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/PreviewHostStorySeekInput.ndjson
            -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/PreviewHostStorySeek.cmake)
    set_tests_properties(PrismatiX.SDK.PreviewHostStorySeek
                         PROPERTIES LABELS "sdk;contract;preview;narrative")

    function(prismatix_add_simple_test target source test_name labels link_target)
        add_executable(${target} ${source})
        target_link_libraries(${target} PRIVATE ${link_target})
        prismatix_configure_target(${target})
        add_test(NAME ${test_name} COMMAND ${target})
        set_tests_properties(${test_name} PROPERTIES LABELS "${labels}")
    endfunction()

    prismatix_add_simple_test(
        PrismatiXSaveLoadIntegrationTests
        Tests/Integration/SaveLoadIntegrationTests.cpp
        PrismatiX.Integration.Runtime.SaveLoad "integration;runtime;save" PrismatiXRuntime)
    prismatix_add_simple_test(
        PrismatiXDialogueScenarioIntegrationTests
        Tests/Integration/DialogueScenarioIntegrationTests.cpp
        PrismatiX.Integration.Runtime.DialogueScenario
        "integration;runtime;dialogue;scenario" PrismatiXRuntime)
    prismatix_add_simple_test(
        PrismatiXBacklogStableIdentityTests
        Tests/BacklogStableIdentityTests.cpp
        PrismatiX.Integration.Player.BacklogStableIdentity
        "integration;player;runtime;backlog;rollback;accessibility"
        PrismatiXRuntime)
    prismatix_add_simple_test(
        PrismatiXBehaviorIntegrationTests
        Tests/Integration/BehaviorIntegrationTests.cpp
        PrismatiX.Integration.Runtime.Behavior "integration;runtime;behavior"
        PrismatiXRuntime)
    prismatix_add_simple_test(
        PrismatiXJavaScriptHostIntegrationTests
        Tests/Integration/JavaScriptHostIntegrationTests.cpp
        PrismatiX.Integration.Runtime.JavaScriptHost
        "integration;runtime;script;javascript"
        PrismatiXRuntime)
    # A native extension library (layer 2) the test signs and loads.
    add_library(PrismatiXTestNativePlugin SHARED
        Tests/Fixtures/NativePlugin/TestNativePlugin.c)
    target_include_directories(PrismatiXTestNativePlugin PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")
    set_target_properties(PrismatiXTestNativePlugin PROPERTIES
        C_VISIBILITY_PRESET hidden PREFIX "")
    add_dependencies(PrismatiXJavaScriptHostIntegrationTests
        PrismatiXTestNativePlugin)
    target_compile_definitions(PrismatiXJavaScriptHostIntegrationTests PRIVATE
        PRISMATIX_TEST_NATIVE_PLUGIN="$<TARGET_FILE:PrismatiXTestNativePlugin>")
    prismatix_add_simple_test(
        PrismatiXAnimationIntegrationTests
        Tests/Integration/AnimationIntegrationTests.cpp
        PrismatiX.Integration.Runtime.Animation "integration;runtime;animation"
        PrismatiXRuntime)
    prismatix_add_simple_test(
        PrismatiXRuntimeResourceIntegrationTests
        Tests/Integration/RuntimeResourceIntegrationTests.cpp
        PrismatiX.Integration.Runtime.Resources "integration;runtime;resource"
        PrismatiXRuntime)
    prismatix_add_simple_test(
        PrismatiXVideoPlayerAcceptanceTests
        Tests/Integration/VideoPlayerAcceptanceTests.cpp
        PrismatiX.Acceptance.Runtime.Video
        "acceptance;runtime;video;ffmpeg;native" PrismatiXRuntime)
    target_compile_definitions(PrismatiXVideoPlayerAcceptanceTests PRIVATE
        PRISMATIX_VIDEO_FIXTURE_BASE64="${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/Video/h264-aac.mp4.b64")
    prismatix_add_simple_test(
        PrismatiXGraphicsIntegrationTests
        Tests/Integration/GraphicsIntegrationTests.cpp
        PrismatiX.Integration.Runtime.Graphics
        "integration;runtime;graphics;gpu;native" PrismatiXRuntime)
    target_compile_definitions(PrismatiXGraphicsIntegrationTests PRIVATE
        PRISMATIX_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../Resources")
    prismatix_add_simple_test(
        PrismatiXStageGraphParticleIntegrationTests
        Tests/Integration/StageGraphParticleIntegrationTests.cpp
        PrismatiX.Integration.Runtime.StageGraphParticles
        "integration;runtime;graphics;stage;particles;save" PrismatiXRuntime)
    target_compile_definitions(PrismatiXStageGraphParticleIntegrationTests PRIVATE
        PRISMATIX_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../Resources")
    prismatix_add_simple_test(
        PrismatiXAssetPreloadIntegrationTests
        Tests/Integration/AssetPreloadIntegrationTests.cpp
        PrismatiX.Integration.Runtime.AssetPreload
        "integration;runtime;graphics;asset;native" PrismatiXRuntime)
    prismatix_add_simple_test(
        PrismatiXAccessibilityIntegrationTests
        Tests/Integration/AccessibilityIntegrationTests.cpp
        PrismatiX.Integration.Runtime.Accessibility
        "integration;runtime;ui;accessibility" PrismatiXRuntime)
    if(WIN32)
        set(PRISMATIX_PLATFORM_ACCESSIBILITY_TEST
            PrismatiX.Integration.Runtime.Accessibility.WindowsUIA)
        set(PRISMATIX_PLATFORM_ACCESSIBILITY_LABELS
            "integration;runtime;ui;accessibility;native;windows")
    elseif(APPLE)
        set(PRISMATIX_PLATFORM_ACCESSIBILITY_TEST
            PrismatiX.Integration.Runtime.Accessibility.NSAccessibility)
        set(PRISMATIX_PLATFORM_ACCESSIBILITY_LABELS
            "integration;runtime;ui;accessibility;native;macos")
    else()
        set(PRISMATIX_PLATFORM_ACCESSIBILITY_TEST
            PrismatiX.Integration.Runtime.Accessibility.ATSPI)
        set(PRISMATIX_PLATFORM_ACCESSIBILITY_LABELS
            "integration;runtime;ui;accessibility;native;linux")
    endif()
    prismatix_add_simple_test(
        PrismatiXPlatformAccessibilityIntegrationTests
        Tests/Integration/PlatformAccessibilityIntegrationTests.cpp
        "${PRISMATIX_PLATFORM_ACCESSIBILITY_TEST}"
        "${PRISMATIX_PLATFORM_ACCESSIBILITY_LABELS}"
        PrismatiXRuntime)

    prismatix_add_simple_test(
        PrismatiXUISchemaContractTests Tests/Contract/UISchemaContractTests.cpp
        PrismatiX.Contract.UI.Schema "contract;runtime" PrismatiXRuntime)
    prismatix_add_simple_test(
        PrismatiXStyleContractTests Tests/Contract/StyleContractTests.cpp
        PrismatiX.Contract.UI.Style "contract;runtime" PrismatiXRuntime)
    prismatix_add_simple_test(
        PrismatiXActionContractTests Tests/Contract/ActionContractTests.cpp
        PrismatiX.Contract.UI.Action "contract;runtime" PrismatiXRuntime)
    prismatix_add_simple_test(
        PrismatiXAnimationContractTests Tests/Contract/AnimationContractTests.cpp
        PrismatiX.Contract.UI.Animation "contract;runtime" PrismatiXRuntime)
    prismatix_add_simple_test(
        PrismatiXSplashContractTests Tests/Contract/SplashContractTests.cpp
        PrismatiX.Contract.Runtime.Splash "contract;runtime;splash" PrismatiXRuntime)
    prismatix_add_simple_test(
        PrismatiXSplashSequencePlayerTests
        Tests/Unit/Runtime/SplashSequencePlayerTests.cpp
        PrismatiX.Unit.Runtime.Splash "unit;runtime;splash" PrismatiXRuntime)
    prismatix_add_simple_test(
        PrismatiXFileSourceTests
        Tests/Unit/IO/FileSourceTests.cpp
        PrismatiX.Unit.IO.FileSource "unit;io;archive;android" PrismatiXRuntime)
    if(PRISMATIX_MEDIA_ENCODERS_FOUND)
        prismatix_add_simple_test(
            PrismatiXMediaTranscoderTests
            Tests/Unit/Packager/MediaTranscoderTests.cpp
            PrismatiX.Unit.Packager.MediaTranscoder "unit;packager;media" PrismatiXRuntime)
        target_link_libraries(PrismatiXMediaTranscoderTests PRIVATE PrismatiXMediaTranscoder)
    endif()
    prismatix_add_simple_test(
        PrismatiXAppLifecycleTests
        Tests/Unit/Platform/AppLifecycleTests.cpp
        PrismatiX.Unit.Platform.AppLifecycle
        "unit;platform;lifecycle;audio;android" PrismatiXRuntime)
    prismatix_add_simple_test(
        PrismatiXProcessWatchTests
        Tests/Unit/Platform/ProcessWatchTests.cpp
        PrismatiX.Unit.Platform.ProcessWatch
        "unit;platform;preview;player" PrismatiXRuntime)
    prismatix_add_simple_test(
        PrismatiXTouchGestureTests
        Tests/Unit/Platform/TouchGestureTests.cpp
        PrismatiX.Unit.Platform.TouchGestures
        "unit;platform;input;touch;android" PrismatiXRuntime)
    prismatix_add_simple_test(
        PrismatiXProfileRulesTests
        Tests/Unit/Runtime/ProfileRulesTests.cpp
        PrismatiX.Unit.Runtime.ProfileRules "unit;runtime;settings" PrismatiXRuntime)
    prismatix_add_simple_test(
        PrismatiXPlayerNavigationTests
        Tests/Unit/Runtime/PlayerNavigationTests.cpp
        PrismatiX.Unit.Player.Navigation "unit;player;settings" PrismatiXRuntime)
    prismatix_add_simple_test(
        PrismatiXJobSystemTests
        Tests/Unit/Core/JobSystemTests.cpp
        PrismatiX.Unit.Core.JobSystem "unit;core;jobs;threads" PrismatiXRuntime)
    prismatix_add_simple_test(
        PrismatiXPlayerBootSplashIntegrationTests
        Tests/Integration/PlayerBootSplashIntegrationTests.cpp
        PrismatiX.Integration.Player.BootSplash "integration;runtime;splash"
        PrismatiXRuntime)

    if(PRISMATIX_PERFORMANCE_TESTS)
        prismatix_add_simple_test(
            PrismatiXPreviewPerformanceBudgetTests
            Tests/PreviewPerformanceBudgetTests.cpp
            PrismatiX.Performance.PreviewBudgets
            "performance;preview;runtime;budget" PrismatiXRuntimeCore)
        set_tests_properties(PrismatiX.Performance.PreviewBudgets
                             PROPERTIES TIMEOUT 60)
    endif()

    add_executable(PrismatiXSoakTests Tests/SoakTests.cpp)
    target_link_libraries(PrismatiXSoakTests PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXSoakTests)
    if(PRISMATIX_STRESS_TESTS)
        add_test(NAME PrismatiX.Stress.Soak COMMAND PrismatiXSoakTests)
        set_tests_properties(PrismatiX.Stress.Soak PROPERTIES
                             LABELS "stress;performance;assets;timeline"
                             TIMEOUT 120)
    endif()

    add_executable(PrismatiXCommercialMegaVNTortureTests
                   Tests/CommercialMegaVNTortureTests.cpp)
    target_link_libraries(PrismatiXCommercialMegaVNTortureTests PRIVATE
                          PrismatiXRuntime PrismatiXPackagerSDK)
    prismatix_configure_target(PrismatiXCommercialMegaVNTortureTests)
    if(PRISMATIX_STRESS_TESTS)
        add_test(NAME PrismatiX.Stress.CommercialMegaVN
                 COMMAND PrismatiXCommercialMegaVNTortureTests)
        set_tests_properties(PrismatiX.Stress.CommercialMegaVN PROPERTIES
                             LABELS "stress;commercial;packager;runtime;save;rollback;locale;soak"
                             TIMEOUT 180)
    endif()
    if(PRISMATIX_HEAVY_STRESS_TESTS)
        add_test(NAME PrismatiX.Stress.CommercialMegaVN.Heavy
                 COMMAND PrismatiXCommercialMegaVNTortureTests --heavy)
        set_tests_properties(PrismatiX.Stress.CommercialMegaVN.Heavy PROPERTIES
                             LABELS "stress;heavy;commercial;packager;runtime;save;rollback;locale;soak"
                             TIMEOUT 600)
    endif()

    add_test(NAME PrismatiX.Architecture.SDK
             COMMAND ${CMAKE_COMMAND}
                     -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/SDKArchitectureGate.cmake)
    set_tests_properties(PrismatiX.Architecture.SDK PROPERTIES
                         LABELS "architecture;sdk")

    # A crashed assertion can open a native debug dialog on Windows and leave
    # the process alive forever. Every registered release-gate test therefore
    # has an explicit upper bound; long stress coverage gets its own budget.
    # A test that declared its own bound above keeps it.
    get_property(PRISMATIX_REGISTERED_TESTS DIRECTORY PROPERTY TESTS)
    foreach(PRISMATIX_TEST IN LISTS PRISMATIX_REGISTERED_TESTS)
        get_test_property(${PRISMATIX_TEST} TIMEOUT PRISMATIX_TEST_TIMEOUT)
        if(NOT PRISMATIX_TEST_TIMEOUT)
            set_tests_properties(${PRISMATIX_TEST} PROPERTIES TIMEOUT 60)
        endif()
    endforeach()
    # The packaged-Player journey packs six distributions and launches the
    # Player eight times: about three minutes on a Windows release build, more
    # under sanitizers or a parallel run. Its waits end as soon as each step
    # is logged, so this bound only limits how long a hung run takes to fail.
    set_tests_properties(PrismatiX.Player.CatalogNativeAcceptance
                         PROPERTIES TIMEOUT 1200)
    # The weekly RSS gate streams a real multi-GiB media entry. Regular runs
    # use 32 MiB through the identical path; only input size changes.
    set_tests_properties(PrismatiX.SDK.PackagerContract PROPERTIES TIMEOUT 600)
    if(PRISMATIX_STRESS_TESTS)
        set_tests_properties(PrismatiX.Stress.Soak PROPERTIES TIMEOUT 120)
        set_tests_properties(PrismatiX.Stress.CommercialMegaVN PROPERTIES TIMEOUT 180)
    endif()
    if(PRISMATIX_HEAVY_STRESS_TESTS)
        set_tests_properties(PrismatiX.Stress.CommercialMegaVN.Heavy
                             PROPERTIES TIMEOUT 600)
    endif()

    add_custom_target(PrismatiXSDKTests DEPENDS
        PrismatiXPackager
        PrismatiXUiTypeRegistryManifest
        PrismatiXInputKeyRegistryManifest
        PrismatiXSDKReferenceDocument
        PrismatiXPackagerContractTests
        PrismatiXCharacterResourcesContractTests
        PrismatiXGameCatalogResourcesContractTests
        PrismatiXUiContractTests
        PrismatiXUiRuntimeAdapterTests
        PrismatiXUiTypeRegistryContractTests
        PrismatiXSDKManifestContractTests
        PrismatiXRuntimeIrContractTests
        PrismatiXRuntimeIrRuntimeAdapterTests
        PrismatiXRuntimeAssetResolverTests
        PrismatiXPreviewSessionContractTests
        PrismatiXPreviewHost
        PrismatiXSaveLoadIntegrationTests
        PrismatiXDialogueScenarioIntegrationTests
        PrismatiXBehaviorIntegrationTests
        PrismatiXJavaScriptHostIntegrationTests
        PrismatiXAnimationIntegrationTests
        PrismatiXRuntimeResourceIntegrationTests
        PrismatiXVideoPlayerAcceptanceTests
        PrismatiXGraphicsIntegrationTests
        PrismatiXStageGraphParticleIntegrationTests
        PrismatiXAssetPreloadIntegrationTests
        PrismatiXAccessibilityIntegrationTests
        PrismatiXUISchemaContractTests
        PrismatiXStyleContractTests
        PrismatiXActionContractTests
        PrismatiXAnimationContractTests
        PrismatiXSplashContractTests
        PrismatiXSplashSequencePlayerTests
        PrismatiXPlayerBootSplashIntegrationTests
        PrismatiXPlayer
        PrismatiXPlayerQA)
    if(TARGET PrismatiXPlatformAccessibilityIntegrationTests)
        add_dependencies(PrismatiXSDKTests
                         PrismatiXPlatformAccessibilityIntegrationTests)
    endif()
    add_dependencies(PrismatiXSDKTests
                     PrismatiXPlayerCatalogNativeAcceptanceTests)

    add_custom_target(PrismatiXRequiredTests DEPENDS PrismatiXSDKTests)
    add_custom_target(PrismatiXRegularTests DEPENDS PrismatiXRequiredTests)
endif()

if(PRISMATIX_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "PRISMATIX_BUILD_FUZZERS requires Clang")
    endif()
    add_executable(PrismatiXParserFuzz Tests/ParserFuzz.cpp)
    target_link_libraries(PrismatiXParserFuzz PRIVATE PrismatiXRuntime)
    target_compile_options(PrismatiXParserFuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(PrismatiXParserFuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    if(BUILD_TESTING)
        add_test(NAME PrismatiX.Fuzz.Parser.Smoke
                 COMMAND PrismatiXParserFuzz -runs=100)
        set_tests_properties(PrismatiX.Fuzz.Parser.Smoke PROPERTIES LABELS "fuzz")
    endif()
endif()
