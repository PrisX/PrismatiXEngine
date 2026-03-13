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
    static std::unordered_map<std::string, int> fontSizeByKey;
    static std::unordered_map<TTF_Font*, std::string> fontReverseMap;
    static std::unordered_map<std::string, TTF_Font*> outlineFontCache;

    static TTF_Font* GetOrCreateOutlineFont(TTF_Font* baseFont, int outlineSize);

public:
    static constexpr int FONT_OVERSAMPLE = 2;
    static TTF_Font* LoadFont(const std::string& fileName, int fontSize);
    static void Draw(SDL_Renderer* ren, TTF_Font* font, const std::string& text, SDL_Color color, int x, int y);
    static void DrawCentered(SDL_Renderer* ren, TTF_Font* font, const std::string& text, SDL_Color color, SDL_Rect bounds);
    static void DrawWithOutline(SDL_Renderer* ren, TTF_Font* font, const std::string& text, SDL_Color textColor, SDL_Color outlineColor, int outlineSize, int x, int y, Uint32 wrapLength = 0, Uint8 alpha = 255, bool shadow = false);
    static void DrawWithOutlineCentered(SDL_Renderer* ren, TTF_Font* font, const std::string& text, SDL_Color textColor, SDL_Color outlineColor, int outlineSize, SDL_Rect bounds, Uint8 alpha = 255, bool shadow = false);
    static void Clean();
};