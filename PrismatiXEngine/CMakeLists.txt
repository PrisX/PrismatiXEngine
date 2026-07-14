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

add_executable(PrismatiXUIMigrate Applications/UIMigrateMain.cpp)
target_link_libraries(PrismatiXUIMigrate PRIVATE PrismatiXRuntime)
prismatix_configure_target(PrismatiXUIMigrate)


file(GLOB_RECURSE PRISMATIX_EDITOR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Editor/*.cpp"
)

if(PRISMATIX_EDITOR_SOURCES)
    add_executable(PrismatiXEditor Applications/EditorMain.cpp ${PRISMATIX_EDITOR_SOURCES})
    target_include_directories(PrismatiXEditor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(PrismatiXEditor PRIVATE
        PrismatiXRuntime
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
    add_executable(PrismatiXCoreTests Tests/CommercialAcceptanceTests.cpp)
    target_link_libraries(PrismatiXCoreTests PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXCoreTests)
    add_test(NAME PrismatiX.Commercial.Acceptance COMMAND PrismatiXCoreTests)

    add_executable(PrismatiXImportTests Tests/ImportServiceTests.cpp
                   Editor/Assets/ImportService.cpp
                   Editor/Workspace/DocumentRegistry.cpp)
    target_link_libraries(PrismatiXImportTests PRIVATE PrismatiXRuntime)
    if(PRISMATIX_STRESS_TESTS)
        target_compile_definitions(PrismatiXImportTests PRIVATE PRISMATIX_STRESS_TESTS=1)
    endif()
    prismatix_configure_target(PrismatiXImportTests)
    add_test(NAME PrismatiX.Editor.Import COMMAND PrismatiXImportTests)

    add_executable(PrismatiXBuildE2ETests Tests/BuildE2ETests.cpp
                   Editor/Build/BuildService.cpp Editor/Build/ExportProfile.cpp
                   Editor/Project/ProjectService.cpp)
    target_link_libraries(PrismatiXBuildE2ETests PRIVATE PrismatiXRuntime)
    prismatix_configure_target(PrismatiXBuildE2ETests)
    add_dependencies(PrismatiXBuildE2ETests PrismatiXPlayer)
    add_test(NAME PrismatiX.Build.E2E COMMAND PrismatiXBuildE2ETests)
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
