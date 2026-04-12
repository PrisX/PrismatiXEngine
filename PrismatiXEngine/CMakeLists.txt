cmake_minimum_required(VERSION 3.15)
project(PrismatiXEngine LANGUAGES CXX)

set(CORE_SOURCES
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

add_library(PrismatiXCore STATIC ${CORE_SOURCES})

option(PDX_SECRET_KEY "Secret key for PDX archives" "")
target_compile_definitions(PrismatiXCore PUBLIC PDX_SECRET_KEY="${PDX_SECRET_KEY}")

target_include_directories(PrismatiXCore PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Models
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Services
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Systems
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/VN
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/VN/Commands
    ${CMAKE_CURRENT_SOURCE_DIR}/Utils
)
 
target_link_libraries(PrismatiXCore PUBLIC
    SDL2::SDL2
    SDL2::SDL2main
    SDL2_image::SDL2_image
    SDL2_ttf::SDL2_ttf
    SDL2_mixer::SDL2_mixer
    spdlog::spdlog
    ${LUA_LIBRARIES}
    sol2::sol2
)

add_executable(PrismatiXEngine main.cpp)
target_link_libraries(PrismatiXEngine PRIVATE PrismatiXCore)

add_custom_command(
    TARGET PrismatiXEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/Scripts
            $<TARGET_FILE_DIR:PrismatiXEngine>/Data/Scripts
)

set(EDITOR_SOURCES
    Editor/main.cpp
    Editor/EditorApp.cpp
    ../ThirdParty/imgui/imgui.cpp
    ../ThirdParty/imgui/imgui_draw.cpp
    ../ThirdParty/imgui/imgui_widgets.cpp
    ../ThirdParty/imgui/imgui_tables.cpp
    ../ThirdParty/imgui/imgui_demo.cpp
    ../ThirdParty/imgui/backends/imgui_impl_sdl2.cpp
    ../ThirdParty/imgui/backends/imgui_impl_sdlrenderer2.cpp
    ../ThirdParty/imgui-node-editor/imgui_node_editor.cpp
    ../ThirdParty/imgui-node-editor/imgui_node_editor_api.cpp
    ../ThirdParty/imgui-node-editor/imgui_canvas.cpp
    ../ThirdParty/imgui-node-editor/crude_json.cpp
)

add_executable(PrismatiXEditor ${EDITOR_SOURCES})

target_include_directories(PrismatiXEditor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Models
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Services
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Systems
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/VN
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/VN/Commands
    ${CMAKE_CURRENT_SOURCE_DIR}/Utils
    ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/imgui-node-editor
)

target_link_libraries(PrismatiXEditor PRIVATE PrismatiXCore SDL2::SDL2 SDL2::SDL2main SDL2_image::SDL2_image SDL2_ttf::SDL2_ttf SDL2_mixer::SDL2_mixer spdlog::spdlog)
add_custom_command(
    TARGET PrismatiXEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/Scripts
            $<TARGET_FILE_DIR:PrismatiXEditor>/Data/Scripts
)
