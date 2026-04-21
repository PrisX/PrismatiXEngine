#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <string>

#include "Utils/LRUCache.h"

namespace PrismatiX::Services {
class ResourceManager;
}

namespace PrismatiX::Systems {

struct TextCacheKey {
    std::string text;
    TTF_Font* font;
    SDL_Color color;
    SDL_Color outlineColor;
    int outlineSize;
    Uint32 wrapLength;

    bool operator==(const TextCacheKey& other) const {
        return text == other.text && font == other.font && outlineSize == other.outlineSize && wrapLength == other.wrapLength && std::memcmp(&color, &other.color, sizeof(Uint32)) == 0 && std::memcmp(&outlineColor, &other.outlineColor, sizeof(Uint32)) == 0;
    }
};

} // namespace PrismatiX::Systems

namespace std {
template <>
struct hash<PrismatiX::Systems::TextCacheKey> {
    std::size_t operator()(const PrismatiX::Systems::TextCacheKey& key) const {
        Uint32 colorValue = 0;
        Uint32 outlineColorValue = 0;
        std::memcpy(&colorValue, &key.color, sizeof(Uint32));
        std::memcpy(&outlineColorValue, &key.outlineColor, sizeof(Uint32));

        // Hash combine: https://stackoverflow.com/a/2595226

        std::size_t h = std::hash<std::string>{}(key.text);
        h ^= std::hash<void*>{}(key.font) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<Uint32>{}(colorValue) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<Uint32>{}(outlineColorValue) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(key.outlineSize) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<Uint32>{}(key.wrapLength) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};
}  // namespace std

namespace PrismatiX::Systems {

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
    RenderSystem(SDL_Renderer* ren, PrismatiX::Services::ResourceManager& resMgr);
    ~RenderSystem();

    // Texture Rendering
    void DrawTexture(SDL_Texture* tex, int x, int y, float scale = 1.0f);
    void DrawTexture(SDL_Texture* tex, int x, int y, int w, int h, Uint8 alpha = 255);
    SDL_Rect DrawTextureAuto(SDL_Texture* tex, DisplayMode mode = DisplayMode::TopLeft, Uint8 alpha = 255, int offsetX = 0, int offsetY = 0, float scale = 1.0f, ShadowConfig shadow = {});
    void DrawRoundedRect(float x, float y, float w, float h, float radius, SDL_Color color);

    // Text Rendering
    void DrawText(TTF_Font* font, const std::string& text, SDL_Color color, int x, int y);
    void DrawTextCentered(TTF_Font* font, const std::string& text, SDL_Color color, SDL_Rect bounds);
    void DrawTextWithOutline(TTF_Font* font, const std::string& text, SDL_Color textColor, SDL_Color outlineColor, int outlineSize, int x, int y, Uint32 wrapLength = 0, Uint8 alpha = 255, bool shadow = false);
    void DrawTextWithOutlineCentered(TTF_Font* font, const std::string& text, SDL_Color textColor, SDL_Color outlineColor, int outlineSize, SDL_Rect bounds, Uint8 alpha = 255, bool shadow = false);

    void SetCameraOffset(int x, int y) {
        cameraOffsetX = x;
        cameraOffsetY = y;
    }

    SDL_Renderer* GetRenderer() const { return renderer; }

private:
    SDL_Renderer* renderer;
    PrismatiX::Services::ResourceManager& resourceManager;

    int cameraOffsetX = 0;
    int cameraOffsetY = 0;

    PrismatiX::Utils::LRUCache<TextCacheKey, CachedTexture> textCache;

    SDL_Surface* RenderTextSurface(TTF_Font* font, const std::string& text, SDL_Color color, Uint32 wrapLength);
};

} // namespace PrismatiX::Systems
