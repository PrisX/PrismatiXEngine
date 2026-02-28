# CMakeList.txt: PrismatiXEngine 的 CMake 專案

# Executable
add_executable (PrismatiXEngine 
"main.cpp"
"Core/PrismatiXEngine.cpp"
"Core/PrismatiXEngine.h"
"Managers/TextureManager.cpp"
"Managers/TextureManager.h"
 "Managers/TextManager.h" "Managers/TextManager.cpp" "Components/DialogueBox.h" "Controllers/DialogueController.h" "Managers/ScriptManager.h" "Managers/AudioManager.h" "Managers/AudioManager.cpp" "SecretKey.h" "Managers/ArchiveManager.h" "Managers/ArchiveManager.cpp" "Managers/ConfigManager.h" "Managers/ConfigManager.cpp" "Controllers/SplashController.h")

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