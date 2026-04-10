cmake_minimum_required(VERSION 3.15)
project(PrismatiXEngine LANGUAGES CXX)

# Source files
set(PRISMATIX_SOURCES
    main.cpp
    Core/Engine.cpp
    Core/LuaBindings.cpp
    Core/Services/ResourceManager.cpp
    Core/Services/GameState.cpp
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
    SecretKey.h
)

# Header files search paths
target_include_directories(PrismatiXEngine PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Models
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Interfaces
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Services
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Systems
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/VN
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/VN/Commands
    ${CMAKE_CURRENT_SOURCE_DIR}/UI
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
            $<TARGET_FILE_DIR:PrismatiXEngine>/Scripts
)
