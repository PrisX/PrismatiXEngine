cmake_minimum_required(VERSION 3.15)
project(PrismatiXEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
set(PRISMATIX_SOURCES
    main.cpp
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

add_executable(PrismatiXEngine
    ${PRISMATIX_SOURCES}
)

option(PDX_SECRET_KEY "Secret key for PDX archives" "")
target_compile_definitions(PrismatiXEngine PRIVATE PDX_SECRET_KEY="${PDX_SECRET_KEY}")

# Header files search paths
target_include_directories(PrismatiXEngine PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Models
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Services
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Systems
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/VN
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/VN/Commands
    ${CMAKE_CURRENT_SOURCE_DIR}/Utils
    ${LUA_INCLUDE_DIR}
)
 
# Libraries
target_link_libraries(PrismatiXEngine PRIVATE
    SDL2::SDL2
    SDL2::SDL2main
    SDL2_image::SDL2_image
    SDL2_ttf::SDL2_ttf
    SDL2_mixer::SDL2_mixer
    spdlog::spdlog
    ${LUA_LIBRARIES}
    sol2::sol2
)

# Post-build copy scripts
add_custom_command(
    TARGET PrismatiXEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/Scripts
            $<TARGET_FILE_DIR:PrismatiXEngine>/Data/Scripts
)
