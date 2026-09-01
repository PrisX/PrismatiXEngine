#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct SDL_Renderer;
struct SDL_Texture;

namespace px::graphics {

struct FrameCaptureSummary {
    int width = 0;
    int height = 0;
    std::size_t sampledColors = 0;
    std::uint64_t hash = 1469598103934665603ull;
    [[nodiscard]] bool Valid() const {
        return width > 0 && height > 0 && sampledColors > 0;
    }
};

[[nodiscard]] std::vector<std::uint8_t> CaptureThumbnailPng(SDL_Renderer* renderer, int w, int h);
[[nodiscard]] FrameCaptureSummary CaptureFrameSummary(
    SDL_Renderer* renderer, std::size_t maximumSamples = 4096);

[[nodiscard]] SDL_Texture* DecodePngTexture(SDL_Renderer* renderer,
                                            const std::vector<std::uint8_t>& png);

}
