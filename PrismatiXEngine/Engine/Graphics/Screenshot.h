#pragma once

#include <cstdint>
#include <vector>

struct SDL_Renderer;
struct SDL_Texture;

namespace px::graphics {

[[nodiscard]] std::vector<std::uint8_t> CaptureThumbnailPng(SDL_Renderer* renderer, int w, int h);

[[nodiscard]] SDL_Texture* DecodePngTexture(SDL_Renderer* renderer,
                                            const std::vector<std::uint8_t>& png);

}
