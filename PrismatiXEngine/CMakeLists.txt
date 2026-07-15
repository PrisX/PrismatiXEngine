cmake_minimum_required(VERSION 3.21)


file(GLOB_RECURSE PRISMATIX_RUNTIME_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/*.cpp"
)

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

file(GLOB_RECURSE PRISMATIX_EDITOR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Editor/*.cpp"
)

set(PRISMATIX_UI_DESIGNER_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Workspace/EditHistory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/Canvas/CanvasInteractionController.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/Canvas/HitTestService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/Canvas/SnapEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/Components/ComponentService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/DesignerCommandService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/DesignerDiagnostics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/DesignerDocumentView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/DocumentChangeSet.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/DesignerNodeFactory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/DesignerSelection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/DesignerUiState.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/Preview/PreviewFixture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/Preview/PreviewChangePlanner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/PropertyEditorRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/UIDesignerSession.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/UISceneDocument.cpp
)

add_library(PrismatiXUIDesignerCore STATIC ${PRISMATIX_UI_DESIGNER_CORE_SOURCES})
target_include_directories(PrismatiXUIDesignerCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(PrismatiXUIDesignerCore PUBLIC PrismatiXRuntime)
prismatix_configure_target(PrismatiXUIDesignerCore)

list(REMOVE_ITEM PRISMATIX_EDITOR_SOURCES ${PRISMATIX_UI_DESIGNER_CORE_SOURCES})

if(PRISMATIX_EDITOR_SOURCES)
    add_executable(PrismatiXEditor Applications/EditorMain.cpp ${PRISMATIX_EDITOR_SOURCES})
    target_include_directories(PrismatiXEditor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(PrismatiXEditor PRIVATE
        PrismatiXRuntime
        PrismatiXUIDesignerCore
        PrismatiXNodeEditor
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
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Fonts/NotoSansTC-Bold.ttf
                $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets/UIFont.ttf
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/imgui-node-editor/assets/BlueprintBackground.png
                $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets/NodeHeader.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Editor/HUD/dialog-stage.png
                $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets/HUD/dialog-stage.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Editor/HUD/notice.png
                $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets/HUD/notice.png
        COMMENT "Preparing versioned PrismatiX Editor assets"
        VERBATIM
    )
    add_dependencies(PrismatiXPrepareEditorAssets PrismatiXEditor)
    add_custom_target(PrismatiXPackageEditor DEPENDS PrismatiXPrepareEditorAssets)
endif()

if(BUILD_TESTING)
    function(prismatix_add_simple_test target source test_name labels link_target)
        add_executable(${target} ${source})
        target_link_libraries(${target} PRIVATE ${link_target})
        prismatix_configure_target(${target})
        add_test(NAME ${test_name} COMMAND ${target})
        set_tests_properties(${test_name} PROPERTIES LABELS "${labels}")
    endfunction()

    add_executable(PrismatiXRuntimeSubsystemIntegrationTests
                   Tests/Integration/RuntimeSubsystemIntegrationTests.cpp)
    target_link_libraries(PrismatiXRuntimeSubsystemIntegrationTests
                          PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXRuntimeSubsystemIntegrationTests)
    add_test(NAME PrismatiX.Integration.Runtime.Subsystems
             COMMAND PrismatiXRuntimeSubsystemIntegrationTests)
    set_tests_properties(PrismatiX.Integration.Runtime.Subsystems PROPERTIES
                         LABELS "integration;runtime")

    add_executable(PrismatiXUIDesignerIntegrationTests
                   Tests/Integration/UIDesignerIntegrationTests.cpp)
    target_link_libraries(PrismatiXUIDesignerIntegrationTests
                          PRIVATE PrismatiXUIDesignerCore)
    prismatix_configure_target(PrismatiXUIDesignerIntegrationTests)
    add_test(NAME PrismatiX.Integration.UI.Designer
             COMMAND PrismatiXUIDesignerIntegrationTests)
    set_tests_properties(PrismatiX.Integration.UI.Designer PROPERTIES
                         LABELS "integration;designer")

    prismatix_add_simple_test(
        PrismatiXDesignerSelectionTests
        Tests/Unit/Editor/UIDesigner/DesignerSelectionTests.cpp
        PrismatiX.Unit.Designer.Selection "unit;designer" PrismatiXUIDesignerCore)
    prismatix_add_simple_test(
        PrismatiXDesignerDocumentViewTests
        Tests/Unit/Editor/UIDesigner/DesignerDocumentViewTests.cpp
        PrismatiX.Unit.Designer.DocumentView "unit;designer" PrismatiXUIDesignerCore)
    prismatix_add_simple_test(
        PrismatiXDesignerCommandServiceTests
        Tests/Unit/Editor/UIDesigner/DesignerCommandServiceTests.cpp
        PrismatiX.Unit.Designer.Commands "unit;designer" PrismatiXUIDesignerCore)
    prismatix_add_simple_test(
        PrismatiXUIDesignerSessionTests
        Tests/Unit/Editor/UIDesigner/UIDesignerSessionTests.cpp
        PrismatiX.Unit.Designer.Session "unit;designer" PrismatiXUIDesignerCore)
    prismatix_add_simple_test(
        PrismatiXPropertyTransactionTests
        Tests/Unit/Editor/UIDesigner/PropertyTransactionTests.cpp
        PrismatiX.Unit.Designer.Transactions "unit;designer" PrismatiXUIDesignerCore)
    prismatix_add_simple_test(
        PrismatiXHitTestServiceTests
        Tests/Unit/Editor/UIDesigner/HitTestServiceTests.cpp
        PrismatiX.Unit.Designer.HitTest "unit;designer" PrismatiXUIDesignerCore)
    prismatix_add_simple_test(
        PrismatiXSnapEngineTests
        Tests/Unit/Editor/UIDesigner/SnapEngineTests.cpp
        PrismatiX.Unit.Designer.Snap "unit;designer" PrismatiXUIDesignerCore)
    prismatix_add_simple_test(
        PrismatiXDesignerDiagnosticsTests
        Tests/Unit/Editor/UIDesigner/DesignerDiagnosticsTests.cpp
        PrismatiX.Unit.Designer.Diagnostics "unit;designer" PrismatiXUIDesignerCore)
    prismatix_add_simple_test(
        PrismatiXCanvasInteractionControllerTests
        Tests/Unit/Editor/UIDesigner/CanvasInteractionControllerTests.cpp
        PrismatiX.Unit.Designer.Canvas "unit;designer" PrismatiXUIDesignerCore)
    prismatix_add_simple_test(
        PrismatiXComponentServiceTests
        Tests/Unit/Editor/UIDesigner/ComponentServiceTests.cpp
        PrismatiX.Unit.Designer.Components "unit;designer" PrismatiXUIDesignerCore)
    prismatix_add_simple_test(
        PrismatiXDesignerUiStateTests
        Tests/Unit/Editor/UIDesigner/DesignerUiStateTests.cpp
        PrismatiX.Unit.Designer.UIState "unit;designer" PrismatiXUIDesignerCore)
    prismatix_add_simple_test(
        PrismatiXPreviewPlanningTests
        Tests/Unit/Editor/UIDesigner/PreviewPlanningTests.cpp
        PrismatiX.Unit.Designer.PreviewPlanning "unit;designer"
        PrismatiXUIDesignerCore)

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
        PrismatiXUIPreviewIntegrationTests
        Tests/Integration/UIPreviewIntegrationTests.cpp
        PrismatiX.Integration.UI.Preview "integration;designer;runtime"
        PrismatiXUIDesignerCore)

    add_executable(PrismatiXEditorWorkspaceIntegrationTests
                   Tests/Integration/EditorWorkspaceIntegrationTests.cpp
                   Editor/Workspace/DocumentRegistry.cpp)
    target_link_libraries(PrismatiXEditorWorkspaceIntegrationTests
                          PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXEditorWorkspaceIntegrationTests)
    add_test(NAME PrismatiX.Integration.Editor.Workspace
             COMMAND PrismatiXEditorWorkspaceIntegrationTests)
    set_tests_properties(PrismatiX.Integration.Editor.Workspace PROPERTIES
                         LABELS "integration;designer")

    add_executable(PrismatiXPerformanceTests Tests/PerformanceTests.cpp)
    target_link_libraries(PrismatiXPerformanceTests PRIVATE PrismatiXUIDesignerCore)
    prismatix_configure_target(PrismatiXPerformanceTests)
    if(PRISMATIX_PERFORMANCE_TESTS)
        add_test(NAME PrismatiX.Performance.Scenarios COMMAND PrismatiXPerformanceTests)
        set_tests_properties(PrismatiX.Performance.Scenarios PROPERTIES LABELS "performance")
    endif()

    add_executable(PrismatiXSoakTests Tests/SoakTests.cpp)
    target_link_libraries(PrismatiXSoakTests PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXSoakTests)
    if(PRISMATIX_STRESS_TESTS)
        add_test(NAME PrismatiX.Stress.Soak COMMAND PrismatiXSoakTests)
        set_tests_properties(PrismatiX.Stress.Soak PROPERTIES LABELS "stress")
    endif()

    add_test(NAME PrismatiX.Architecture.Gates
             COMMAND ${CMAKE_COMMAND}
                     -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ArchitectureGate.cmake)
    set_tests_properties(PrismatiX.Architecture.Gates PROPERTIES LABELS "architecture")

    add_executable(PrismatiXImportTests Tests/Integration/AssetImportIntegrationTests.cpp
                   Editor/Assets/ImportService.cpp)
    target_link_libraries(PrismatiXImportTests PRIVATE PrismatiXRuntime)
    if(PRISMATIX_STRESS_TESTS)
        target_compile_definitions(PrismatiXImportTests PRIVATE PRISMATIX_STRESS_TESTS=1)
    endif()
    prismatix_configure_target(PrismatiXImportTests)
    add_test(NAME PrismatiX.Editor.Import COMMAND PrismatiXImportTests)
    if(PRISMATIX_STRESS_TESTS)
        set_tests_properties(PrismatiX.Editor.Import PROPERTIES
                             LABELS "integration;designer;stress")
    else()
        set_tests_properties(PrismatiX.Editor.Import PROPERTIES
                             LABELS "integration;designer")
    endif()

    add_executable(PrismatiXBuildE2ETests Tests/E2E/BuildPipelineE2ETests.cpp
                   Editor/Build/BuildService.cpp Editor/Build/ExportProfile.cpp
                   Editor/Project/ProjectService.cpp)
    target_link_libraries(PrismatiXBuildE2ETests PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXBuildE2ETests)
    add_dependencies(PrismatiXBuildE2ETests PrismatiXPlayer)
    add_custom_target(PrismatiXPrepareBuildE2EAssets
        COMMAND ${CMAKE_COMMAND} -E make_directory
                $<TARGET_FILE_DIR:PrismatiXBuildE2ETests>/EditorAssets/HUD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Fonts/NotoSansTC-Bold.ttf
                $<TARGET_FILE_DIR:PrismatiXBuildE2ETests>/EditorAssets/UIFont.ttf
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Editor/HUD/dialog-stage.png
                $<TARGET_FILE_DIR:PrismatiXBuildE2ETests>/EditorAssets/HUD/dialog-stage.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Editor/HUD/notice.png
                $<TARGET_FILE_DIR:PrismatiXBuildE2ETests>/EditorAssets/HUD/notice.png
        COMMENT "Preparing versioned Build E2E fixture assets"
        VERBATIM
    )
    add_dependencies(PrismatiXBuildE2ETests PrismatiXPrepareBuildE2EAssets)
    add_test(NAME PrismatiX.Build.E2E COMMAND PrismatiXBuildE2ETests)
    set_tests_properties(PrismatiX.Build.E2E PROPERTIES LABELS "e2e")

    add_custom_target(PrismatiXRequiredPRTests DEPENDS
        PrismatiXRuntimeSubsystemIntegrationTests
        PrismatiXUIDesignerIntegrationTests
        PrismatiXDesignerSelectionTests
        PrismatiXDesignerDocumentViewTests
        PrismatiXDesignerCommandServiceTests
        PrismatiXUIDesignerSessionTests
        PrismatiXPropertyTransactionTests
        PrismatiXHitTestServiceTests
        PrismatiXSnapEngineTests
        PrismatiXDesignerDiagnosticsTests
        PrismatiXCanvasInteractionControllerTests
        PrismatiXComponentServiceTests
        PrismatiXDesignerUiStateTests
        PrismatiXPreviewPlanningTests
        PrismatiXUISchemaContractTests
        PrismatiXStyleContractTests
        PrismatiXActionContractTests
        PrismatiXAnimationContractTests
        PrismatiXUIPreviewIntegrationTests
        PrismatiXEditorWorkspaceIntegrationTests
        PrismatiXImportTests)
    add_custom_target(PrismatiXRegularTests DEPENDS
        PrismatiXRequiredPRTests PrismatiXBuildE2ETests)
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
