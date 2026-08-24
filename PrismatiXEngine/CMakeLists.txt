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
    if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
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
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/PreviewContractManifest.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/preview-contract.json"
    @ONLY
)

add_library(PrismatiXRuntimeContracts STATIC
    Engine/SDK/CharacterResources.cpp
    Engine/SDK/GameCatalogResources.cpp
    Engine/SDK/RuntimeIr.cpp
    Engine/SDK/StudioUi.cpp
)
add_library(PrismatiX::RuntimeContracts ALIAS PrismatiXRuntimeContracts)
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
    Engine/SDK/UiTypeRegistry.cpp
)
add_library(PrismatiX::SDK ALIAS PrismatiXSDK)
prismatix_configure_target(PrismatiXSDK)
target_include_directories(PrismatiXSDK PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/PrismatiX>
)
target_link_libraries(PrismatiXSDK PUBLIC
    PrismatiXRuntimeContracts
    zstd::libzstd
)
if(TARGET MbedTLS::mbedcrypto)
    target_link_libraries(PrismatiXSDK PUBLIC MbedTLS::mbedcrypto)
elseif(TARGET MbedTLS::tfpsacrypto)
    target_link_libraries(PrismatiXSDK PUBLIC MbedTLS::tfpsacrypto)
elseif(TARGET MbedTLS::mbedtls)
    target_link_libraries(PrismatiXSDK PUBLIC MbedTLS::mbedtls)
endif()

install(TARGETS PrismatiXSDK EXPORT PrismatiXSDKTargets
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
install(FILES
    "${PRISMATIX_GENERATED_INCLUDE_DIR}/Engine/SDK/ContractVersions.h"
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/PrismatiX/Engine/SDK
    COMPONENT PrismatiXSDK
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/PrismatiXSDKConfigVersion.cmake"
    VERSION 0.1.0
    COMPATIBILITY SameMajorVersion
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
endif()


file(GLOB_RECURSE PRISMATIX_RUNTIME_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/*.cpp"
)
list(FILTER PRISMATIX_RUNTIME_CORE_SOURCES EXCLUDE REGEX "/Engine/SDK/")
list(FILTER PRISMATIX_RUNTIME_CORE_SOURCES EXCLUDE REGEX "/Engine/Runtime\\.cpp$")
list(FILTER PRISMATIX_RUNTIME_CORE_SOURCES EXCLUDE REGEX "/Engine/Platform/")
list(FILTER PRISMATIX_RUNTIME_CORE_SOURCES EXCLUDE REGEX "/Engine/Accessibility/SpeechService\\.cpp$")
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
target_include_directories(PrismatiXRuntimeCore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/pl_mpeg
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

if(NOT EMSCRIPTEN)
    file(GLOB_RECURSE PRISMATIX_NATIVE_PLATFORM_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Engine/IO/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Platform/*.cpp"
    )
    list(APPEND PRISMATIX_NATIVE_PLATFORM_SOURCES
        Engine/Runtime.cpp
        Engine/Accessibility/SpeechService.cpp
        Engine/Video/VideoPlayer.cpp
        Engine/Resources/AssetRegistry.cpp
        Engine/UI/UITypeManifest.cpp
        Engine/Progression/GameSettings.cpp
        Engine/Progression/GlobalProfileStore.cpp
        Engine/Progression/Persist.cpp
        Engine/Progression/SaveSystem.cpp
    )
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
        ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/pl_mpeg
    )
    target_link_libraries(PrismatiXRuntimePlatformNative PUBLIC
        PrismatiXRuntimeCore
        PrismatiXSDK
        prismatix_mbedcrypto
        zstd::libzstd
        ${FFMPEG_LIBRARIES}
    )

    add_library(PrismatiXRuntime INTERFACE)
    target_link_libraries(PrismatiXRuntime INTERFACE
        PrismatiXRuntimePlatformNative)
endif()

if(NOT EMSCRIPTEN)
add_executable(PrismatiXPlayer
    Applications/PlayerMain.cpp
    Applications/Player/PlayerApp.cpp
)
target_link_libraries(PrismatiXPlayer PRIVATE PrismatiXRuntime)
if(WIN32)
    target_link_libraries(PrismatiXPlayer PRIVATE sapi)
endif()
prismatix_configure_target(PrismatiXPlayer)

add_executable(PrismatiXPackager Applications/PackagerMain.cpp)
target_link_libraries(PrismatiXPackager PRIVATE PrismatiXSDK)
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
install(FILES "${PRISMATIX_UI_TYPE_MANIFEST}"
    DESTINATION .
    COMPONENT PrismatiXSDK
)
install(TARGETS PrismatiXPackager PrismatiXPreviewHost PrismatiXPlayer
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT PrismatiXRuntime
)
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
endif()

if(PRISMATIX_BUILD_WASM_PREVIEW)
    add_executable(PrismatiXPreviewWasm
        Applications/PreviewWasmMain.cpp
        Engine/Runtime.cpp
        Engine/Platform/Input.cpp
        Engine/Platform/Window.cpp
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
        "-sENVIRONMENT=web,worker"
        "-sFILESYSTEM=1"
        "-sNO_EXIT_RUNTIME=0"
        "$<$<CONFIG:Release>:-O3>"
        "$<$<CONFIG:Release>:-gsource-map>"
        "-sEXPORTED_RUNTIME_METHODS=['cwrap','UTF8ToString','FS']"
        "-sEXPORTED_FUNCTIONS=['_malloc','_free','_px_preview_create','_px_preview_apply','_px_preview_patch','_px_preview_control','_px_preview_resize','_px_preview_tick','_px_preview_drain_events','_px_preview_destroy']"
        "SHELL:--embed-file \"${PROJECT_SOURCE_DIR}/Resources/Fonts/NotoSansTC-Bold.ttf@/project/Resources/Fonts/NotoSansTC-Bold.ttf\""
        "--source-map-base" "./"
    )
    add_custom_command(TARGET PrismatiXPreviewWasm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_BINARY_DIR}/preview-contract.json"
            "$<TARGET_FILE_DIR:PrismatiXPreviewWasm>/preview-contract.json"
        VERBATIM)
endif()

if(BUILD_TESTING AND NOT EMSCRIPTEN)
    # Contract and integration tests use assert() as their executable test
    # primitive. Keep those checks active in Release/RelWithDebInfo quality
    # gates; otherwise NDEBUG removes both validation and assertion operands.
    if(MSVC)
        add_compile_options(/UNDEBUG)
    else()
        add_compile_options(-UNDEBUG)
    endif()

    add_executable(PrismatiXPreviewProtocolV2Tests
                   Tests/PreviewProtocolV2Tests.cpp)
    target_link_libraries(PrismatiXPreviewProtocolV2Tests PRIVATE
                          PrismatiXRuntimeCore)
    prismatix_configure_target(PrismatiXPreviewProtocolV2Tests)
    add_test(NAME PrismatiX.Preview.WasmProtocolV2
             COMMAND PrismatiXPreviewProtocolV2Tests)
    set_tests_properties(PrismatiX.Preview.WasmProtocolV2
                         PROPERTIES LABELS "preview;wasm;contract")

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
    target_link_libraries(PrismatiXPackagerContractTests PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXPackagerContractTests)
    add_test(NAME PrismatiX.SDK.PackagerContract
             COMMAND PrismatiXPackagerContractTests)
    set_tests_properties(PrismatiX.SDK.PackagerContract
                         PROPERTIES LABELS "sdk;contract;packager")

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

    if(WIN32)
        add_executable(PrismatiXPlayerCatalogNativeAcceptanceTests
                       Tests/PlayerCatalogNativeAcceptanceTests.cpp)
        target_link_libraries(PrismatiXPlayerCatalogNativeAcceptanceTests
                              PRIVATE PrismatiXRuntime user32 gdi32)
        prismatix_configure_target(PrismatiXPlayerCatalogNativeAcceptanceTests)
        add_dependencies(PrismatiXPlayerCatalogNativeAcceptanceTests
                         PrismatiXPlayer)
        add_test(
            NAME PrismatiX.Player.CatalogNativeAcceptance
            COMMAND PrismatiXPlayerCatalogNativeAcceptanceTests
                    $<TARGET_FILE:PrismatiXPlayer>
                    ${CMAKE_CURRENT_SOURCE_DIR}/../Content
                    ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Branding/PrismatiXEngine_Logo.png)
        set_tests_properties(
            PrismatiX.Player.CatalogNativeAcceptance
            PROPERTIES
                LABELS "native;windows;e2e;player;characters"
                RUN_SERIAL TRUE
                TIMEOUT 60)
    endif()

    add_test(NAME PrismatiX.SDK.PackagerCliContract
        COMMAND ${CMAKE_COMMAND}
            -DPACKAGER=$<TARGET_FILE:PrismatiXPackager>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/PackagerCliContract.cmake)
    set_tests_properties(PrismatiX.SDK.PackagerCliContract
                         PROPERTIES LABELS "sdk;contract;packager")

    add_executable(PrismatiXStudioUiContractTests Tests/StudioUiContractTests.cpp)
    target_link_libraries(PrismatiXStudioUiContractTests PRIVATE PrismatiXSDK)
    prismatix_configure_target(PrismatiXStudioUiContractTests)
    add_test(NAME PrismatiX.SDK.StudioUiContract COMMAND PrismatiXStudioUiContractTests)
    set_tests_properties(PrismatiX.SDK.StudioUiContract PROPERTIES LABELS "sdk;contract;ui")

    add_executable(PrismatiXStudioUiRuntimeAdapterTests Tests/StudioUiRuntimeAdapterTests.cpp)
    target_link_libraries(PrismatiXStudioUiRuntimeAdapterTests PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXStudioUiRuntimeAdapterTests)
    add_test(NAME PrismatiX.SDK.StudioUiRuntimeAdapter COMMAND PrismatiXStudioUiRuntimeAdapterTests)
    set_tests_properties(PrismatiX.SDK.StudioUiRuntimeAdapter PROPERTIES LABELS "sdk;runtime;contract;ui")

    add_executable(PrismatiXUiTypeRegistryContractTests
                   Tests/UiTypeRegistryContractTests.cpp)
    target_link_libraries(PrismatiXUiTypeRegistryContractTests
                          PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXUiTypeRegistryContractTests)
    add_test(NAME PrismatiX.SDK.UiTypeRegistryContract
             COMMAND PrismatiXUiTypeRegistryContractTests)
    set_tests_properties(PrismatiX.SDK.UiTypeRegistryContract
                         PROPERTIES LABELS "sdk;runtime;contract;ui")

    add_executable(PrismatiXSDKManifestContractTests
                   Tests/SDKManifestContractTests.cpp)
    target_link_libraries(PrismatiXSDKManifestContractTests
                          PRIVATE PrismatiXRuntime)
    target_compile_definitions(PrismatiXSDKManifestContractTests PRIVATE
        "PRISMATIX_SDK_MANIFEST_PATH=\"${CMAKE_CURRENT_BINARY_DIR}/PrismatiXSDKManifest.json\"")
    prismatix_configure_target(PrismatiXSDKManifestContractTests)
    add_test(NAME PrismatiX.SDK.ManifestContract
             COMMAND PrismatiXSDKManifestContractTests)
    set_tests_properties(PrismatiX.SDK.ManifestContract PROPERTIES
                         LABELS "sdk;contract;architecture")

    add_executable(PrismatiXRuntimeIrContractTests Tests/RuntimeIrContractTests.cpp)
    target_link_libraries(PrismatiXRuntimeIrContractTests PRIVATE PrismatiXSDK)
    prismatix_configure_target(PrismatiXRuntimeIrContractTests)
    add_test(NAME PrismatiX.SDK.RuntimeIrContract COMMAND PrismatiXRuntimeIrContractTests)
    set_tests_properties(PrismatiX.SDK.RuntimeIrContract PROPERTIES LABELS "sdk;contract")

    add_executable(PrismatiXRuntimeIrRuntimeAdapterTests
        Tests/RuntimeIrRuntimeAdapterTests.cpp)
    target_link_libraries(PrismatiXRuntimeIrRuntimeAdapterTests PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXRuntimeIrRuntimeAdapterTests)
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

    add_test(NAME PrismatiX.Architecture.SDK
             COMMAND ${CMAKE_COMMAND}
                     -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/SDKArchitectureGate.cmake)
    set_tests_properties(PrismatiX.Architecture.SDK PROPERTIES
                         LABELS "architecture;sdk")

    add_custom_target(PrismatiXSDKTests DEPENDS
        PrismatiXPackager
        PrismatiXUiTypeRegistryManifest
        PrismatiXPackagerContractTests
        PrismatiXCharacterResourcesContractTests
        PrismatiXGameCatalogResourcesContractTests
        PrismatiXStudioUiContractTests
        PrismatiXStudioUiRuntimeAdapterTests
        PrismatiXUiTypeRegistryContractTests
        PrismatiXSDKManifestContractTests
        PrismatiXRuntimeIrContractTests
        PrismatiXRuntimeIrRuntimeAdapterTests
        PrismatiXRuntimeAssetResolverTests
        PrismatiXPreviewHost
        PrismatiXSaveLoadIntegrationTests
        PrismatiXDialogueScenarioIntegrationTests
        PrismatiXBehaviorIntegrationTests
        PrismatiXJavaScriptHostIntegrationTests
        PrismatiXAnimationIntegrationTests
        PrismatiXRuntimeResourceIntegrationTests
        PrismatiXUISchemaContractTests
        PrismatiXStyleContractTests
        PrismatiXActionContractTests
        PrismatiXAnimationContractTests
        PrismatiXSplashContractTests
        PrismatiXSplashSequencePlayerTests
        PrismatiXPlayerBootSplashIntegrationTests
        PrismatiXPlayer)
    if(WIN32)
        add_dependencies(PrismatiXSDKTests
                         PrismatiXPlayerCatalogNativeAcceptanceTests)
    endif()

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
