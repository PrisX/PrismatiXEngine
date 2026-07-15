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
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/Canvas/DesignerTools.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/Canvas/HitTestService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/Canvas/SnapEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/Components/ComponentService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/DesignerCommandService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/DesignerDiagnostics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/DesignerDocumentView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/DocumentChangeSet.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/DesignerNodeFactory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor/Tools/UIDesigner/DesignerSelection.cpp
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

    add_custom_command(TARGET PrismatiXEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets/HUD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Fonts/NotoSansTC-Bold.ttf
                $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets/UIFont.ttf
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/imgui-node-editor/assets/BlueprintBackground.png
                $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets/NodeHeader.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../hudtemp/dialog-stage.png
                $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets/HUD/dialog-stage.png
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../hudtemp/notice.png
                $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets/HUD/notice.png
    )
endif()

if(BUILD_TESTING)
    add_executable(PrismatiXRuntimeContractTests Tests/RuntimeContractTests.cpp)
    target_link_libraries(PrismatiXRuntimeContractTests PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXRuntimeContractTests)
    add_test(NAME PrismatiX.Runtime.Contracts COMMAND PrismatiXRuntimeContractTests)
    set_tests_properties(PrismatiX.Runtime.Contracts PROPERTIES LABELS "contract;runtime")

    add_executable(PrismatiXUIDesignerCoreTests Tests/UIDesignerCoreTests.cpp)
    target_link_libraries(PrismatiXUIDesignerCoreTests PRIVATE PrismatiXUIDesignerCore)
    prismatix_configure_target(PrismatiXUIDesignerCoreTests)
    add_test(NAME PrismatiX.UIDesigner.Core COMMAND PrismatiXUIDesignerCoreTests)
    set_tests_properties(PrismatiX.UIDesigner.Core PROPERTIES LABELS "unit;contract;designer")

    add_executable(PrismatiXEditorWorkspaceTests Tests/EditorWorkspaceTests.cpp
                   Editor/Workspace/DocumentRegistry.cpp)
    target_link_libraries(PrismatiXEditorWorkspaceTests PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXEditorWorkspaceTests)
    add_test(NAME PrismatiX.Editor.Workspace COMMAND PrismatiXEditorWorkspaceTests)
    set_tests_properties(PrismatiX.Editor.Workspace PROPERTIES LABELS "unit;contract;designer")

    add_executable(PrismatiXPerformanceTests Tests/PerformanceTests.cpp)
    target_link_libraries(PrismatiXPerformanceTests PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXPerformanceTests)
    if(PRISMATIX_STRESS_TESTS)
        add_test(NAME PrismatiX.Performance.Scenarios COMMAND PrismatiXPerformanceTests)
        set_tests_properties(PrismatiX.Performance.Scenarios PROPERTIES LABELS "performance;stress")
    endif()

    add_test(NAME PrismatiX.Architecture.Gates
             COMMAND ${CMAKE_COMMAND}
                     -DROOT=${CMAKE_CURRENT_SOURCE_DIR}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ArchitectureGate.cmake)
    set_tests_properties(PrismatiX.Architecture.Gates PROPERTIES LABELS "unit;contract;designer")

    add_executable(PrismatiXImportTests Tests/ImportServiceTests.cpp
                   Editor/Assets/ImportService.cpp)
    target_link_libraries(PrismatiXImportTests PRIVATE PrismatiXRuntime)
    if(PRISMATIX_STRESS_TESTS)
        target_compile_definitions(PrismatiXImportTests PRIVATE PRISMATIX_STRESS_TESTS=1)
    endif()
    prismatix_configure_target(PrismatiXImportTests)
    add_test(NAME PrismatiX.Editor.Import COMMAND PrismatiXImportTests)
    set_tests_properties(PrismatiX.Editor.Import PROPERTIES LABELS "integration;designer")

    add_executable(PrismatiXBuildE2ETests Tests/BuildE2ETests.cpp
                   Editor/Build/BuildService.cpp Editor/Build/ExportProfile.cpp
                   Editor/Project/ProjectService.cpp)
    target_link_libraries(PrismatiXBuildE2ETests PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXBuildE2ETests)
    add_dependencies(PrismatiXBuildE2ETests PrismatiXPlayer)
    add_test(NAME PrismatiX.Build.E2E COMMAND PrismatiXBuildE2ETests)
    set_tests_properties(PrismatiX.Build.E2E PROPERTIES LABELS "e2e")
endif()

if(PRISMATIX_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "PRISMATIX_BUILD_FUZZERS requires Clang")
    endif()
    add_executable(PrismatiXParserFuzz Tests/ParserFuzz.cpp)
    target_link_libraries(PrismatiXParserFuzz PRIVATE PrismatiXRuntime)
    target_compile_options(PrismatiXParserFuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(PrismatiXParserFuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
