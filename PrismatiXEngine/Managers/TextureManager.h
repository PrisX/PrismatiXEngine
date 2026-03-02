#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <string>
#include <unordered_map>

class TextureManager {
public:
	enum class DisplayMode {
		TopLeft,
		Top,
		Bottom,
		Left,
		Right,
		Center,
		FitWidthBottom,
		Fit,
		Fill
	};

	static SDL_Texture* LoadTexture(const std::string& fileName, SDL_Renderer* ren); // 繼續傳址owo
	static void Draw(SDL_Texture* tex, SDL_Renderer* ren, int x, int y, float scale); // 縮放
	static void Draw(SDL_Texture* tex, SDL_Renderer* ren, int x, int y, int w, int h, Uint8 alpha = 255); // 給立繪
	static SDL_Rect DrawAuto(SDL_Texture* tex, SDL_Renderer* ren, DisplayMode mode = DisplayMode::TopLeft, Uint8 alpha = 255);
	static void CleanCache();
private:
	static std::unordered_map<std::string, SDL_Texture*> textureCache; // Cache
};