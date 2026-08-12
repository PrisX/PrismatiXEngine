cmake_minimum_required(VERSION 3.21)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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

add_library(PrismatiXSDK STATIC
    Engine/SDK/CharacterResources.cpp
    Engine/SDK/GameCatalogResources.cpp
    Engine/SDK/Packager.cpp
    Engine/SDK/RuntimeIr.cpp
    Engine/SDK/StudioUi.cpp
    Engine/SDK/UiTypeRegistry.cpp
)
add_library(PrismatiX::SDK ALIAS PrismatiXSDK)
prismatix_configure_target(PrismatiXSDK)
target_include_directories(PrismatiXSDK PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/PrismatiX>
)
target_link_libraries(PrismatiXSDK PUBLIC
    nlohmann_json::nlohmann_json
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
install(DIRECTORY Engine/SDK
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/PrismatiX/Engine
    COMPONENT PrismatiXSDK
    FILES_MATCHING PATTERN "*.h"
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


file(GLOB_RECURSE PRISMATIX_RUNTIME_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/*.cpp"
)
list(FILTER PRISMATIX_RUNTIME_SOURCES EXCLUDE REGEX "/Engine/SDK/")

add_library(PrismatiXRuntime STATIC ${PRISMATIX_RUNTIME_SOURCES})
prismatix_configure_target(PrismatiXRuntime)

target_include_directories(PrismatiXRuntime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine
    ${LUA_INCLUDE_DIR}
)
target_include_directories(PrismatiXRuntime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/pl_mpeg
)

target_link_libraries(PrismatiXRuntime PUBLIC
    SDL3::SDL3
    SDL3_image::SDL3_image
    SDL3_ttf::SDL3_ttf
    SDL3_mixer::SDL3_mixer
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    prismatix_mbedcrypto
    zstd::libzstd
    ${LUA_LIBRARIES}
    sol2::sol2
    ${FFMPEG_LIBRARIES}
    PrismatiXSDK
)
target_include_directories(PrismatiXRuntime PRIVATE ${FFMPEG_INCLUDE_DIRS})

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

if(PRISMATIX_BUILD_LEGACY_EDITOR)
    file(GLOB_RECURSE PRISMATIX_EDITOR_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/Editor/*.cpp"
    )

if(PRISMATIX_EDITOR_SOURCES)
    add_executable(PrismatiXEditor Applications/EditorMain.cpp ${PRISMATIX_EDITOR_SOURCES})
    target_include_directories(PrismatiXEditor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(PrismatiXEditor PRIVATE
        PrismatiXRuntime
        PrismatiXImGui
    )
    prismatix_configure_target(PrismatiXEditor)
    if(WIN32)
        target_link_libraries(PrismatiXEditor PRIVATE Ole32 Shell32 Dbghelp)
    endif()

    # Compile/link and distributable asset preparation are intentionally separate.
    # A missing package resource must be reported as packaging, not as a C++ link
    # or test-execution failure.
    add_custom_target(PrismatiXPrepareEditorAssets
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets/HUD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets/Branding
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Fonts/NotoSansTC-Bold.ttf
                $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets/UIFont.ttf
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Editor/HUD/dialog-stage.png
                $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets/HUD/dialog-stage.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Editor/HUD/notice.png
                $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets/HUD/notice.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Branding/PrismatiXEngine_Logo.png
                $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets/Branding/PrismatiXEngine_Logo.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Branding/logo.wav
                $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets/Branding/logo.wav
        COMMENT "Preparing versioned PrismatiX Editor assets"
        VERBATIM
    )
    add_dependencies(PrismatiXPrepareEditorAssets PrismatiXEditor)
    add_custom_target(PrismatiXPackageEditor DEPENDS PrismatiXPrepareEditorAssets)
endif()
endif()

if(BUILD_TESTING)
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
    add_test(NAME PrismatiX.SDK.PreviewHostLuaDebug
        COMMAND ${CMAKE_COMMAND}
            -DPREVIEW_HOST=$<TARGET_FILE:PrismatiXPreviewHost>
            -DFIXTURE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/PreviewHostLua
            -DINPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/PreviewHostLuaDebugInput.ndjson
            -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/PreviewHostLuaDebug.cmake)
    set_tests_properties(PrismatiX.SDK.PreviewHostLuaDebug
                         PROPERTIES LABELS "sdk;contract;preview;lua;debugger")
    add_test(NAME PrismatiX.SDK.PreviewHostLuaAction
        COMMAND ${CMAKE_COMMAND}
            -DPREVIEW_HOST=$<TARGET_FILE:PrismatiXPreviewHost>
            -DFIXTURE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/PreviewHostLua
            -DINPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/PreviewHostLuaActionInput.ndjson
            -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/PreviewHostLuaAction.cmake)
    set_tests_properties(PrismatiX.SDK.PreviewHostLuaAction
                         PROPERTIES LABELS "sdk;contract;preview;lua;action")
    add_test(NAME PrismatiX.SDK.PreviewHostRuntimeAssets
        COMMAND ${CMAKE_COMMAND}
            -DPREVIEW_HOST=$<TARGET_FILE:PrismatiXPreviewHost>
            -DFIXTURE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/PreviewHostLua
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


    add_executable(PrismatiXSoakTests Tests/SoakTests.cpp)
    target_link_libraries(PrismatiXSoakTests PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXSoakTests)
    if(PRISMATIX_STRESS_TESTS)
        add_test(NAME PrismatiX.Stress.Soak COMMAND PrismatiXSoakTests)
        set_tests_properties(PrismatiX.Stress.Soak PROPERTIES LABELS "stress")
    endif()

    add_test(NAME PrismatiX.Architecture.SDK
             COMMAND ${CMAKE_COMMAND}
                     -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/SDKArchitectureGate.cmake)
    set_tests_properties(PrismatiX.Architecture.SDK PROPERTIES
                         LABELS "architecture;sdk")

    if(PRISMATIX_BUILD_LEGACY_EDITOR)
    add_test(NAME PrismatiX.Architecture.Gates
             COMMAND ${CMAKE_COMMAND}
                     -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ArchitectureGate.cmake)
    set_tests_properties(PrismatiX.Architecture.Gates PROPERTIES LABELS "architecture")
    add_executable(PrismatiXBuildE2ETests Tests/E2E/BuildPipelineE2ETests.cpp
                   Editor/Build/BuildService.cpp Editor/Build/ExportProfile.cpp
                   Editor/Project/ProjectService.cpp)
    target_link_libraries(PrismatiXBuildE2ETests PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXBuildE2ETests)
    add_dependencies(PrismatiXBuildE2ETests PrismatiXPlayer)
    add_custom_target(PrismatiXPrepareBuildE2EAssets
        COMMAND ${CMAKE_COMMAND} -E make_directory
                $<TARGET_FILE_DIR:PrismatiXBuildE2ETests>/EditorAssets/HUD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                $<TARGET_FILE_DIR:PrismatiXBuildE2ETests>/EditorAssets/Branding
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Fonts/NotoSansTC-Bold.ttf
                $<TARGET_FILE_DIR:PrismatiXBuildE2ETests>/EditorAssets/UIFont.ttf
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Editor/HUD/dialog-stage.png
                $<TARGET_FILE_DIR:PrismatiXBuildE2ETests>/EditorAssets/HUD/dialog-stage.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Editor/HUD/notice.png
                $<TARGET_FILE_DIR:PrismatiXBuildE2ETests>/EditorAssets/HUD/notice.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Branding/PrismatiXEngine_Logo.png
                $<TARGET_FILE_DIR:PrismatiXBuildE2ETests>/EditorAssets/Branding/PrismatiXEngine_Logo.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Branding/logo.wav
                $<TARGET_FILE_DIR:PrismatiXBuildE2ETests>/EditorAssets/Branding/logo.wav
        COMMENT "Preparing versioned Build E2E fixture assets"
        VERBATIM
    )
    add_dependencies(PrismatiXBuildE2ETests PrismatiXPrepareBuildE2EAssets)
    add_test(NAME PrismatiX.Build.E2E COMMAND PrismatiXBuildE2ETests)
    set_tests_properties(PrismatiX.Build.E2E PROPERTIES LABELS "e2e")
    endif()

    add_custom_target(PrismatiXSDKTests DEPENDS
        PrismatiXPackager
        PrismatiXPackagerContractTests
        PrismatiXCharacterResourcesContractTests
        PrismatiXGameCatalogResourcesContractTests
        PrismatiXStudioUiContractTests
        PrismatiXStudioUiRuntimeAdapterTests
        PrismatiXUiTypeRegistryContractTests
        PrismatiXRuntimeIrContractTests
        PrismatiXRuntimeIrRuntimeAdapterTests
        PrismatiXRuntimeAssetResolverTests
        PrismatiXPreviewHost
        PrismatiXSaveLoadIntegrationTests
        PrismatiXDialogueScenarioIntegrationTests
        PrismatiXBehaviorIntegrationTests
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

    if(PRISMATIX_BUILD_LEGACY_EDITOR)
        add_custom_target(PrismatiXRequiredTests DEPENDS PrismatiXSDKTests)
        add_custom_target(PrismatiXRegularTests DEPENDS
            PrismatiXRequiredTests PrismatiXBuildE2ETests)
    else()
        add_custom_target(PrismatiXRequiredTests DEPENDS PrismatiXSDKTests)
        add_custom_target(PrismatiXRegularTests DEPENDS PrismatiXRequiredTests)
    endif()
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
