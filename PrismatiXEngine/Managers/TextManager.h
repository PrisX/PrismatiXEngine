#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <string>
#include <unordered_map>
#include <vector>

class ArchiveManager;

class TextManager {
private:
    ArchiveManager& archiveManager;
    std::unordered_map<std::string, TTF_Font*> fontCache;
    std::unordered_map<std::string, std::vector<char>> fontBuffers;
    std::unordered_map<std::string, int> fontSizeByKey;
    std::unordered_map<TTF_Font*, std::string> fontReverseMap;
    std::unordered_map<std::string, TTF_Font*> outlineFontCache;

    TTF_Font* GetOrCreateOutlineFont(TTF_Font* baseFont, int outlineSize);
    SDL_Surface* RenderTextSurface(TTF_Font* font, const std::string& text, SDL_Color color, Uint32 wrapLength);

public:
    TextManager(ArchiveManager& archiveMgr);
    ~TextManager() = default;

    static constexpr int FONT_OVERSAMPLE = 2;
    TTF_Font* LoadFont(const std::string& fileName, int fontSize);
    void Draw(SDL_Renderer* ren, TTF_Font* font, const std::string& text, SDL_Color color, int x, int y);
    void DrawCentered(SDL_Renderer* ren, TTF_Font* font, const std::string& text, SDL_Color color, SDL_Rect bounds);
    void DrawWithOutline(SDL_Renderer* ren, TTF_Font* font, const std::string& text, SDL_Color textColor, SDL_Color outlineColor, int outlineSize, int x, int y, Uint32 wrapLength = 0, Uint8 alpha = 255, bool shadow = false);
    void DrawWithOutlineCentered(SDL_Renderer* ren, TTF_Font* font, const std::string& text, SDL_Color textColor, SDL_Color outlineColor, int outlineSize, SDL_Rect bounds, Uint8 alpha = 255, bool shadow = false);
    void Clean();
};