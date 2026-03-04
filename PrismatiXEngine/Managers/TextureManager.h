#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <string>
#include <unordered_map>

class TextureManager {
public:
	enum class DisplayMode {
		TopLeft,
		TopRight,
		BottomLeft,
		BottomRight,
		Top,
		Bottom,
		Left,
		Right,
		Center,
		FitWidthBottom,
		Fit,
		Fill
	};

	struct ShadowConfig {
		bool enabled = false;
		int offsetX = 4;
		int offsetY = 4;
		Uint8 alpha = 100;
	};

	static SDL_Texture* LoadTexture(const std::string& fileName, SDL_Renderer* ren); // 繼續傳址owo
	static void Draw(SDL_Texture* tex, SDL_Renderer* ren, int x, int y, float scale); // 縮放
	static void Draw(SDL_Texture* tex, SDL_Renderer* ren, int x, int y, int w, int h, Uint8 alpha = 255); // 給立繪
	static SDL_Rect DrawAuto(SDL_Texture* tex, SDL_Renderer* ren, DisplayMode mode = DisplayMode::TopLeft, Uint8 alpha = 255, int offsetX = 0, int offsetY = 0, float scale = 1.0f, ShadowConfig shadow = {});
	static void CleanCache();
private:
	static std::unordered_map<std::string, SDL_Texture*> textureCache; // Cache
};