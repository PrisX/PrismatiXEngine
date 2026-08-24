cmake_minimum_required(VERSION 3.21)
project(PrismatiXInstalledSDKConsumer LANGUAGES CXX)

find_package(PrismatiXSDK CONFIG REQUIRED)

if(NOT PrismatiX_PREVIEW_HOST_AVAILABLE OR
   NOT TARGET PrismatiX::PreviewHost OR
   NOT EXISTS "${PrismatiX_PREVIEW_HOST_EXECUTABLE}")
    message(FATAL_ERROR "Installed PrismatiX SDK does not expose PreviewHost")
endif()

add_executable(PrismatiXInstalledSDKConsumer main.cpp)
target_compile_features(PrismatiXInstalledSDKConsumer PRIVATE cxx_std_20)
target_link_libraries(PrismatiXInstalledSDKConsumer PRIVATE PrismatiX::SDK)
target_compile_definitions(PrismatiXInstalledSDKConsumer PRIVATE
    "PRISMATIX_INSTALLED_PREVIEW_HOST=\"${PrismatiX_PREVIEW_HOST_EXECUTABLE}\"")

enable_testing()
add_test(NAME PrismatiX.InstalledSDK.Consumer
         COMMAND PrismatiXInstalledSDKConsumer)
add_test(NAME PrismatiX.InstalledSDK.PreviewHostHello
         COMMAND "${PrismatiX_PREVIEW_HOST_EXECUTABLE}")
set_tests_properties(PrismatiX.InstalledSDK.PreviewHostHello PROPERTIES
    PASS_REGULAR_EXPRESSION "PrismatiXPreviewProtocol|ready|exited"
    TIMEOUT 5)
