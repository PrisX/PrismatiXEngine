#include "Engine/Graphics/Screenshot.h"

#include "Engine/Support/Logger.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <cstring>
#include <algorithm>
#include <unordered_set>

namespace px::graphics {

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

FrameCaptureSummary CaptureFrameSummary(SDL_Renderer* renderer,
                                        const std::size_t maximumSamples) {
    FrameCaptureSummary summary;
    if(!renderer||maximumSamples==0)return summary;
    SDL_Surface* captured=SDL_RenderReadPixels(renderer,nullptr);
    if(!captured){
        PX_LOG_WARN("CaptureFrameSummary: RenderReadPixels failed: {}",
                    SDL_GetError());
        return summary;
    }
    SDL_Surface* rgba=SDL_ConvertSurface(captured,SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(captured);
    if(!rgba)return summary;
    summary.width=rgba->w;
    summary.height=rgba->h;
    const std::size_t count=static_cast<std::size_t>(rgba->w)*
                            static_cast<std::size_t>(rgba->h);
    const std::size_t stride=std::max<std::size_t>(1,count/maximumSamples);
    std::unordered_set<std::uint32_t> colors;
    const auto* bytes=static_cast<const std::uint8_t*>(rgba->pixels);
    for(std::size_t index=0;index<count;index+=stride){
        const auto* row=bytes+(index/static_cast<std::size_t>(rgba->w))*
                              static_cast<std::size_t>(rgba->pitch);
        const auto pixel=reinterpret_cast<const std::uint32_t*>(row)
            [index%static_cast<std::size_t>(rgba->w)];
        colors.insert(pixel);
        summary.hash^=pixel;
        summary.hash*=1099511628211ull;
    }
    summary.sampledColors=colors.size();
    SDL_DestroySurface(rgba);
    return summary;
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
