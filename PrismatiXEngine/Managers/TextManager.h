#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>

class TextManager {
public:
	static TTF_Font* LoadFont(const std::string& fileName, int fontSize);
	static void Draw(SDL_Renderer* ren, TTF_Font* font, const std::string& text, SDL_Color color, int x, int y);
	static void DrawWithOutline(SDL_Renderer* ren, TTF_Font* font, const std::string& text,
		SDL_Color textColor, SDL_Color outlineColor, int outlineSize,
		int x, int y);
};