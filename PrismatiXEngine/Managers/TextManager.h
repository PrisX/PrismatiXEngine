#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <unordered_map>
#include <vector>

class TextManager {
private:
	static std::unordered_map<std::string, TTF_Font*> fontCache;
	static std::unordered_map<std::string, std::vector<char>> fontBuffers;

public:
	static constexpr int FONT_OVERSAMPLE = 2;
	static TTF_Font* LoadFont(const std::string& fileName, int fontSize);
	static void Draw(SDL_Renderer* ren, TTF_Font* font, const std::string& text, SDL_Color color, int x, int y);
	static void DrawWithOutline(SDL_Renderer* ren, TTF_Font* font, const std::string& text,
		SDL_Color textColor, SDL_Color outlineColor, int outlineSize,
		int x, int y, Uint32 wrapLength = 0, Uint8 alpha = 255);
	static void Clean();
};