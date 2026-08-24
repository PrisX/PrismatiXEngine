cmake_minimum_required(VERSION 3.21)
project(PrismatiXInstalledSDKConsumer LANGUAGES CXX)

find_package(PrismatiXSDK CONFIG REQUIRED)

add_executable(PrismatiXInstalledSDKConsumer main.cpp)
target_compile_features(PrismatiXInstalledSDKConsumer PRIVATE cxx_std_20)
target_link_libraries(PrismatiXInstalledSDKConsumer PRIVATE PrismatiX::SDK)

enable_testing()
add_test(NAME PrismatiX.InstalledSDK.Consumer
         COMMAND PrismatiXInstalledSDKConsumer)
