# CMakeList.txt: PrismatiXEngine 的 CMake 專案

# Executable
add_executable (PrismatiXEngine 
"main.cpp"
"Core/PrismatiXEngine.cpp"
"Core/PrismatiXEngine.h"
"Core/LuaBindings.cpp"
"Core/LuaBindings.h"
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
"Managers/VariableManager.h"
"Managers/VariableManager.cpp"
"Managers/UIManager.h"
"Managers/UIManager.cpp"
"Managers/BacklogManager.h"
"Managers/BacklogManager.cpp"
"Managers/SaveManager.h"
"Managers/SaveManager.cpp"
"Utils/TransitionUtils.h"
"Utils/EasingUtils.h"
"SecretKey.h"
)

# Header files
target_include_directories(PrismatiXEngine PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/Managers
    ${CMAKE_CURRENT_SOURCE_DIR}/Controllers
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
    ${LUA_LIBRARIES}
    sol2::sol2
)

if(APPLE)
target_include_directories(PrismatiXEngine PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/Managers
    ${CMAKE_CURRENT_SOURCE_DIR}/Controllers
    ${CMAKE_CURRENT_SOURCE_DIR}/Utils
    ${HOMEBREW_PREFIX}/include
    ${LUA_INCLUDE_DIR}
)

endif()

add_custom_command(
    TARGET PrismatiXEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/Scripts
            $<TARGET_FILE_DIR:PrismatiXEngine>/Scripts
)