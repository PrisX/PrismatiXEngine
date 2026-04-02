#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>

class AssetManager;

struct ShadowConfig {
    bool enabled = false;
    int offsetX = 4;
    int offsetY = 4;
    Uint8 alpha = 100;
};

enum class DisplayMode { TopLeft, TopRight, BottomLeft, BottomRight, Top, Bottom, Left, Right, Center, FitWidthBottom, Fit, Fill };

class RenderSystem {
private:
    SDL_Renderer* renderer;
    AssetManager& assetManager;

    SDL_Surface* RenderTextSurface(TTF_Font* font, const std::string& text, SDL_Color color, Uint32 wrapLength);

public:
    static constexpr int FONT_OVERSAMPLE = 2;

    RenderSystem(SDL_Renderer* ren, AssetManager& assetMgr);
    ~RenderSystem() = default;

    // Texture Rendering
    void DrawTexture(SDL_Texture* tex, int x, int y, float scale = 1.0f);
    void DrawTexture(SDL_Texture* tex, int x, int y, int w, int h, Uint8 alpha = 255);
    SDL_Rect DrawTextureAuto(SDL_Texture* tex, DisplayMode mode = DisplayMode::TopLeft, Uint8 alpha = 255, int offsetX = 0, int offsetY = 0, float scale = 1.0f, ShadowConfig shadow = {});

    // Text Rendering
    void DrawText(TTF_Font* font, const std::string& text, SDL_Color color, int x, int y);
    void DrawTextCentered(TTF_Font* font, const std::string& text, SDL_Color color, SDL_Rect bounds);
    void DrawTextWithOutline(TTF_Font* font, const std::string& text, SDL_Color textColor, SDL_Color outlineColor, int outlineSize, int x, int y, Uint32 wrapLength = 0, Uint8 alpha = 255, bool shadow = false);
    void DrawTextWithOutlineCentered(TTF_Font* font, const std::string& text, SDL_Color textColor, SDL_Color outlineColor, int outlineSize, SDL_Rect bounds, Uint8 alpha = 255, bool shadow = false);
};
