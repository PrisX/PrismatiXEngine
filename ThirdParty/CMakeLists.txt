cmake_minimum_required(VERSION 3.15)

add_library(PrismatiXImGui STATIC
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/misc/cpp/imgui_stdlib.cpp
    imgui/backends/imgui_impl_sdl2.cpp
    imgui/backends/imgui_impl_sdlrenderer2.cpp
)

target_include_directories(PrismatiXImGui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/misc/cpp
)

target_link_libraries(PrismatiXImGui PUBLIC
    SDL2::SDL2
    SDL2::SDL2main
)

add_library(PrismatiXNodeEditor STATIC
    imgui-node-editor/crude_json.cpp
    imgui-node-editor/imgui_canvas.cpp
    imgui-node-editor/imgui_node_editor.cpp
    imgui-node-editor/imgui_node_editor_api.cpp
    imgui-node-editor/utilities/drawing.cpp
    imgui-node-editor/utilities/widgets.cpp
)

target_include_directories(PrismatiXNodeEditor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui-node-editor
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui-node-editor/utilities
)

target_link_libraries(PrismatiXNodeEditor PUBLIC
    PrismatiXImGui
)
