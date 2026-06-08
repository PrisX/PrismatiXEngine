#include "Engine/Gfx/Screenshot.h"

#include "Logger.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <cstring>

namespace px::gfx {

std::vector<std::uint8_t> CaptureThumbnailPng(SDL_Renderer* renderer, int w, int h) {
    SDL_Surface* full = SDL_RenderReadPixels(renderer, nullptr);
    if (!full) {
        PX_LOG_WARN("CaptureThumbnail: RenderReadPixels failed: {}", SDL_GetError());
        return {};
    }
    SDL_Surface* small = SDL_ScaleSurface(full, w, h, SDL_SCALEMODE_LINEAR);
    SDL_DestroySurface(full);
    if (!small) {
        return {};
    }

    SDL_IOStream* io = SDL_IOFromDynamicMem();
    std::vector<std::uint8_t> out;
    if (io && IMG_SavePNG_IO(small, io, /*closeio=*/false)) {
        const Sint64 size = SDL_GetIOSize(io);
        SDL_PropertiesID props = SDL_GetIOProperties(io);
        void* mem = SDL_GetPointerProperty(props, SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, nullptr);
        if (mem && size > 0) {
            out.resize(static_cast<std::size_t>(size));
            std::memcpy(out.data(), mem, out.size());
        }
    }
    if (io) {
        SDL_CloseIO(io);
    }
    SDL_DestroySurface(small);
    return out;
}

SDL_Texture* DecodePngTexture(SDL_Renderer* renderer, const std::vector<std::uint8_t>& png) {
    if (png.empty()) {
        return nullptr;
    }
    SDL_IOStream* io = SDL_IOFromConstMem(png.data(), png.size());
    SDL_Surface* surface = io ? IMG_Load_IO(io, /*closeio=*/true) : nullptr;
    if (!surface) {
        return nullptr;
    }
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    return texture;
}

}
