#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <string>

class TextureManager {
public:
	static SDL_Texture* LoadTexture(const std::string& fileName, SDL_Renderer* ren); // 繼續傳址owo
	static void Draw(SDL_Texture* tex, SDL_Renderer* ren, int x, int y, int w, int h);
};