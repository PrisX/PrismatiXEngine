cmake_minimum_required(VERSION 3.21)
project(PrismatiXDependencySmoke LANGUAGES C CXX)

find_package(SDL3 CONFIG REQUIRED)
find_package(SDL3_image CONFIG REQUIRED)
find_package(SDL3_ttf CONFIG REQUIRED)
find_package(SDL3_mixer CONFIG REQUIRED)

if(NOT TARGET SDL3_image::SDL3_image)
    message(FATAL_ERROR "SDL3_image did not provide SDL3_image::SDL3_image")
endif()

# Static SDL3_image packages traverse their private WebP dependency while the
# config is loaded. This is the macOS/Linux path affected by the WebP/webp
# result-name mismatch. Shared packages (the normal Windows path) do not expose
# or resolve that private dependency, so only assert the targets for static.
if(TARGET SDL3_image::SDL3_image-static)
    foreach(WEBP_TARGET IN ITEMS
            WebP::sharpyuv
            WebP::webpdecoder
            WebP::webp
            WebP::webpdemux
            WebP::libwebpmux)
        if(NOT TARGET ${WEBP_TARGET})
            message(FATAL_ERROR
                "Static SDL3_image WebP support did not resolve ${WEBP_TARGET}")
        endif()
    endforeach()
endif()

message(STATUS "PrismatiX dependency smoke passed for SDL3_image package targets")
