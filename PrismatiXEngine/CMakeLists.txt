# CMakeList.txt: PrismatiXEngine 的 CMake 專案
# 將來源新增到此專案的可執行檔。
add_executable (PrismatiXEngine 
"main.cpp"
"Core/PrismatiXEngine.cpp"
"Core/PrismatiXEngine.h"
"Managers/TextureManager.cpp"
"Managers/TextureManager.h"
)

# 設定標頭檔的搜尋路徑 (指向當前資料夾)
target_include_directories(PrismatiXEngine PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/Managers
)
 
# SDL2
target_link_libraries(PrismatiXEngine PRIVATE
    SDL2::SDL2
    SDL2::SDL2main
    SDL2_image::SDL2_image
    SDL2_ttf::SDL2_ttf
    SDL2_mixer::SDL2_mixer
)