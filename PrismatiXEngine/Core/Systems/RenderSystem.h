#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <string>
#include <memory>
#include "Utils/LRUCache.h"

class ResourceManager;

struct TextCacheKey {
    std::string text;
    TTF_Font* font;
    SDL_Color color;
    SDL_Color outlineColor;
    int outlineSize;
    Uint32 wrapLength;

    bool operator==(const TextCacheKey& o) const {
        return text == o.text && font == o.font && outlineSize == o.outlineSize && wrapLength == o.wrapLength &&
               *((Uint32*)&color) == *((Uint32*)&o.color) && *((Uint32*)&outlineColor) == *((Uint32*)&o.outlineColor);
    }
};

namespace std {
template <>
struct hash<TextCacheKey> {
    std::size_t operator()(const TextCacheKey& k) const {
        size_t h = std::hash<std::string>{}(k.text);
        h ^= std::hash<void*>{}(k.font) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<Uint32>{}(*((Uint32*)&k.color)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(k.outlineSize) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};
}  // namespace std


struct CachedTexture {
    SDL_Texture* texture;
    int w, h;
};

struct ShadowConfig {
    bool enabled = false;
    int offsetX = 4;
    int offsetY = 4;
    Uint8 alpha = 100;
};

enum class DisplayMode { TopLeft, TopRight, BottomLeft, BottomRight, Top, Bottom, Left, Right, Center, FitWidthBottom, Fit, Fill };

class RenderSystem {
public:
    RenderSystem(SDL_Renderer* ren, ResourceManager& resMgr);
    ~RenderSystem();

    // Texture Rendering
    void DrawTexture(SDL_Texture* tex, int x, int y, float scale = 1.0f);
    void DrawTexture(SDL_Texture* tex, int x, int y, int w, int h, Uint8 alpha = 255);
    SDL_Rect DrawTextureAuto(SDL_Texture* tex, DisplayMode mode = DisplayMode::TopLeft, Uint8 alpha = 255, int offsetX = 0, int offsetY = 0, float scale = 1.0f, ShadowConfig shadow = {});

    // Text Rendering
    void DrawText(TTF_Font* font, const std::string& text, SDL_Color color, int x, int y);
    void DrawTextCentered(TTF_Font* font, const std::string& text, SDL_Color color, SDL_Rect bounds);
    void DrawTextWithOutline(TTF_Font* font, const std::string& text, SDL_Color textColor, SDL_Color outlineColor, int outlineSize, int x, int y, Uint32 wrapLength = 0, Uint8 alpha = 255, bool shadow = false);
    void DrawTextWithOutlineCentered(TTF_Font* font, const std::string& text, SDL_Color textColor, SDL_Color outlineColor, int outlineSize, SDL_Rect bounds, Uint8 alpha = 255, bool shadow = false);

    void SetCameraOffset(int x, int y) {
        cameraOffsetX = x;
        cameraOffsetY = y;
    }

private:
    SDL_Renderer* renderer;
    ResourceManager& resourceManager;

    int cameraOffsetX = 0;
    int cameraOffsetY = 0;

    LRUCache<TextCacheKey, CachedTexture> textCache;

    SDL_Surface* RenderTextSurface(TTF_Font* font, const std::string& text, SDL_Color color, Uint32 wrapLength);
};
