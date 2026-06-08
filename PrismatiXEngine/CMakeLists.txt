cmake_minimum_required(VERSION 3.21)


file(GLOB_RECURSE PRISMATIX_RUNTIME_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/*.cpp"
)

add_library(PrismatiXRuntime STATIC ${PRISMATIX_RUNTIME_SOURCES})

target_include_directories(PrismatiXRuntime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Engine
    ${LUA_INCLUDE_DIR}
)

target_link_libraries(PrismatiXRuntime PUBLIC
    SDL3::SDL3
    SDL3_image::SDL3_image
    SDL3_ttf::SDL3_ttf
    SDL3_mixer::SDL3_mixer
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    MbedTLS::mbedcrypto
    zstd::libzstd
    ${LUA_LIBRARIES}
    sol2::sol2
)

add_executable(PrismatiXPlayer Applications/PlayerMain.cpp)
target_link_libraries(PrismatiXPlayer PRIVATE PrismatiXRuntime)


file(GLOB_RECURSE PRISMATIX_EDITOR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Editor/*.cpp"
)

if(PRISMATIX_EDITOR_SOURCES)
    add_executable(PrismatiXEditor Applications/EditorMain.cpp ${PRISMATIX_EDITOR_SOURCES})
    target_include_directories(PrismatiXEditor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(PrismatiXEditor PRIVATE
        PrismatiXRuntime
        PrismatiXImGui
        PrismatiXNodeEditor
    )
    if(WIN32)
        target_link_libraries(PrismatiXEditor PRIVATE Ole32 Shell32)
    endif()

    add_custom_command(TARGET PrismatiXEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../Resources/Fonts/NotoSansTC-Bold.ttf
                $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets/UIFont.ttf
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_SOURCE_DIR}/../ThirdParty/imgui-node-editor/assets/BlueprintBackground.png
                $<TARGET_FILE_DIR:PrismatiXEditor>/EditorAssets/NodeHeader.png
    )
endif()
