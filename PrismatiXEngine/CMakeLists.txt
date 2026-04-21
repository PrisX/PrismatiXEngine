cmake_minimum_required(VERSION 3.15)
project(PrismatiXEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PRISMATIX_RUNTIME_SOURCES
    Core/Engine.cpp
    Core/LuaBindings.cpp
    Core/Lua/LuaEngineAPI.cpp
    Core/Lua/LuaRenderAPI.cpp
    Core/Lua/LuaAudioAPI.cpp
    Core/Lua/LuaStateAPI.cpp
    Core/Lua/LuaVNAPI.cpp
    Core/Services/ResourceManager.cpp
    Core/Services/GameState.cpp
    Core/Services/SaveManager.cpp
    Core/Systems/RenderSystem.cpp
    Core/Systems/AudioSystem.cpp
    Core/VN/VNFlowController.cpp
    Core/VN/VNStage.cpp
    Core/VN/VNDialogueSystem.cpp
    Core/VN/VNScriptParser.cpp
    Core/VN/VNChoiceList.cpp
    Core/VN/Commands/BuiltinCommandHandlers.cpp
)

set(PRISMATIX_EDITOR_SOURCES
    EditorMain.cpp
    Editor/BlueprintEditor.cpp
    Editor/BlueprintEditorDocument.cpp
    Editor/EditorApp.cpp
    Editor/EditorAppPanels.cpp
    Editor/GamePreviewHost.cpp
    Editor/UIDesigner.cpp
)

add_library(PrismatiXRuntime STATIC
    ${PRISMATIX_RUNTIME_SOURCES}
)

target_include_directories(PrismatiXRuntime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Models
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Services
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Systems
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/VN
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/VN/Commands
    ${CMAKE_CURRENT_SOURCE_DIR}/Utils
    ${LUA_INCLUDE_DIR}
)

target_link_libraries(PrismatiXRuntime PUBLIC
    SDL2::SDL2
    SDL2_image::SDL2_image
    SDL2_ttf::SDL2_ttf
    SDL2_mixer::SDL2_mixer
    spdlog::spdlog
    ${LUA_LIBRARIES}
    sol2::sol2
)

add_executable(PrismatiXEngine
    GameMain.cpp
)

target_link_libraries(PrismatiXEngine PRIVATE
    PrismatiXRuntime
    SDL2::SDL2main
)

add_executable(PrismatiXEditor
    ${PRISMATIX_EDITOR_SOURCES}
)

target_include_directories(PrismatiXEditor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor
    ${CMAKE_CURRENT_SOURCE_DIR}/Utils
)

target_link_libraries(PrismatiXEditor PRIVATE
    SDL2::SDL2
    SDL2::SDL2main
    SDL2_image::SDL2_image
    spdlog::spdlog
    PrismatiXRuntime
    PrismatiXImGui
    PrismatiXNodeEditor
)

if (WIN32)
    target_link_libraries(PrismatiXEditor PRIVATE
        Ole32
        Shell32
    )
endif()

# Post-build copy scripts
add_custom_command(
    TARGET PrismatiXEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/Scripts
            $<TARGET_FILE_DIR:PrismatiXEngine>/Data/Scripts
)
