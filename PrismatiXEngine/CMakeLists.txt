# CMakeList.txt: PrismatiXEngine 的 CMake 專案

# Executable
add_executable (PrismatiXEngine 
"main.cpp"
"Core/PrismatiXEngine.cpp"
"Core/PrismatiXEngine.h"
"Components/DialogueBox.h"
"Controllers/SplashController.h"
"Controllers/SplashController.cpp"
"Controllers/DialogueController.h"
"Controllers/DialogueController.cpp"
"Managers/TextureManager.cpp"
"Managers/TextureManager.h"
"Managers/TextManager.h"
"Managers/TextManager.cpp"
"Managers/ScriptManager.h"
"Managers/ScriptManager.cpp"
"Managers/AudioManager.h"
"Managers/AudioManager.cpp"
"Managers/ArchiveManager.h"
"Managers/ArchiveManager.cpp"
"Managers/ConfigManager.h"
"Managers/ConfigManager.cpp"
"Managers/VariableManager.h"
"Managers/VariableManager.cpp"
"Managers/UIManager.h"
"Managers/UIManager.cpp"
"Managers/BacklogManager.h"
"Managers/BacklogManager.cpp"
"Managers/SaveManager.h"
"Managers/SaveManager.cpp"
"SecretKey.h"
 "Components/SaveLoadMenu.h" "Components/MainMenu.h")

# Header files
target_include_directories(PrismatiXEngine PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/Managers
    ${CMAKE_CURRENT_SOURCE_DIR}/Components
    ${CMAKE_CURRENT_SOURCE_DIR}/Controllers
)
 
# SDL2
target_link_libraries(PrismatiXEngine PRIVATE
    SDL2::SDL2
    SDL2::SDL2main
    SDL2_image::SDL2_image
    SDL2_ttf::SDL2_ttf
    SDL2_mixer::SDL2_mixer
)

if(APPLE)
target_include_directories(PrismatiXEngine PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/Managers
    ${CMAKE_CURRENT_SOURCE_DIR}/Components
    ${CMAKE_CURRENT_SOURCE_DIR}/Controllers
    ${HOMEBREW_PREFIX}/include
)
endif()