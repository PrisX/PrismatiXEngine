# CMakeList.txt: PrismatiXEngine 的 CMake 專案

# Executable
add_executable (PrismatiXEngine 
"main.cpp"
"Core/Engine.cpp"
"Core/Engine.h"
"Core/LuaBindings.cpp"
"Core/LuaBindings.h"
"Core/Services/ResourceManager.cpp"
"Core/Services/ResourceManager.h"
"Core/Services/VNScriptParser.cpp"
"Core/Services/VNScriptParser.h"
"Core/Services/GameState.cpp"
"Core/Services/GameState.h"
"Core/Systems/RenderSystem.cpp"
"Core/Systems/RenderSystem.h"
"Core/Systems/AudioSystem.cpp"
"Core/Systems/AudioSystem.h"
"Core/VN/VNFlowController.cpp"
"Core/VN/VNFlowController.h"
"Core/VN/VNStage.cpp"
"Core/VN/VNStage.h"
"Core/VN/VNDialogueSystem.cpp"
"Core/VN/VNDialogueSystem.h"
"Core/VN/Models.cpp"
"Core/VN/Models.h"
"UI/UIManager.cpp"
"UI/UIManager.h"
"Utils/TransitionUtils.h"
"Utils/EasingUtils.h"
"Utils/Logger.h"
"SecretKey.h"
)

# Header files
target_include_directories(PrismatiXEngine PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Services
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Systems
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/VN
    ${CMAKE_CURRENT_SOURCE_DIR}/UI
    ${CMAKE_CURRENT_SOURCE_DIR}/Utils
    ${LUA_INCLUDE_DIR}
)
 
# SDL2
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

add_custom_command(
    TARGET PrismatiXEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/Scripts
            $<TARGET_FILE_DIR:PrismatiXEngine>/Data/Scripts
)
