cmake_minimum_required(VERSION 3.15)
project(PrismatiXEngine LANGUAGES CXX)

set(PRISMATIX_APP_SOURCES
    main.cpp
    Core/Engine.cpp
    Core/LuaBindings.cpp
)

set(PRISMATIX_CORE_SERVICES
    Core/Services/ResourceManager.cpp
    Core/Services/GameState.cpp
    Core/Services/ResourceManager.h
    Core/Services/GameState.h
)

set(PRISMATIX_CORE_SYSTEMS
    Core/Systems/RenderSystem.cpp
    Core/Systems/AudioSystem.cpp
    Core/Systems/RenderSystem.h
    Core/Systems/AudioSystem.h
)

set(PRISMATIX_CORE_VN
    Core/VN/VNFlowController.cpp
    Core/VN/VNStage.cpp
    Core/VN/VNDialogueSystem.cpp
    Core/VN/Models.cpp
    Core/VN/VNFlowController.h
    Core/VN/VNStage.h
    Core/VN/VNDialogueSystem.h
    Core/VN/Models.h
    Core/VN/VNScriptParser.cpp
    Core/VN/VNScriptParser.h
    Core/VN/Commands/BuiltinCommandHandlers.cpp
    Core/VN/Commands/BuiltinCommandHandlers.h
    Core/VN/Commands/ICommandHandler.h
    Core/VN/Commands/VNContext.h
)

set(PRISMATIX_CORE_MODELS
    Core/Models/SaveData.h
    Core/Models/VNCommand.h
)

set(PRISMATIX_CORE_INFRA
    Core/Engine.h
    Core/LuaBindings.h
    Core/ServiceContainer.h
    Core/Interfaces/IRenderer.h
    Core/Interfaces/IAudioPlayer.h
    Core/Interfaces/IResourceLoader.h
    Core/Interfaces/IScene.h
)

set(PRISMATIX_UI
    UI/UIManager.cpp
    UI/UIManager.h
    UI/SceneManager.h
)

set(PRISMATIX_UTILS
    Utils/TransitionUtils.h
    Utils/EasingUtils.h
    Utils/LRUCache.h
    Utils/Logger.h
)

add_executable(PrismatiXEngine
    ${PRISMATIX_APP_SOURCES}
    ${PRISMATIX_CORE_SERVICES}
    ${PRISMATIX_CORE_SYSTEMS}
    ${PRISMATIX_CORE_VN}
    ${PRISMATIX_CORE_MODELS}
    ${PRISMATIX_CORE_INFRA}
    ${PRISMATIX_UI}
    ${PRISMATIX_UTILS}
    SecretKey.h
)

# Header files
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
            $<TARGET_FILE_DIR:PrismatiXEngine>/Scripts
)
