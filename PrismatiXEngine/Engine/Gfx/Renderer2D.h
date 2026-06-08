#pragma once

#include "Engine/Common/Types.h"
#include "Engine/Gfx/AssetCache.h"

#include <string>
#include <unordered_map>

struct SDL_Renderer;
struct SDL_Texture;

namespace px::gfx {

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
    Fill,
};

struct Shadow {
    bool enabled = false;
    int offsetX = 3;
    int offsetY = 3;
    std::uint8_t alpha = 110;
};

class Renderer2D {
public:
    Renderer2D(SDL_Renderer* renderer, AssetCache& assets);

    void SetLogicalSize(int width, int height);
    void GetLogicalSize(int& width, int& height) const;

    void SetCameraOffset(int x, int y) { m_camX = x, m_camY = y; }
    void ResetCamera() { m_camX = 0, m_camY = 0; }

    void DrawRect(const Rect& rect, Color color);
    void DrawRoundedRect(const Rect& rect, float radius, Color color);

    void DrawImage(const std::string& path, const Rect& dst, std::uint8_t alpha = 255);
    Rect DrawImageAuto(const std::string& path, DisplayMode mode, std::uint8_t alpha = 255,
                       int offsetX = 0, int offsetY = 0, float scale = 1.0f, Shadow shadow = {});

    void DrawText(const std::string& text, float x, float y, const std::string& fontPath, int size,
                  Color color, std::uint8_t alpha = 255, int wrap = 0);
    void DrawTextOutline(const std::string& text, float x, float y, const std::string& fontPath,
                         int size, Color textColor, Color outlineColor, int outlineSize,
                         std::uint8_t alpha = 255, bool shadow = false, int wrap = 0);
    [[nodiscard]] Vec2 MeasureText(const std::string& text, const std::string& fontPath, int size,
                                   int wrap = 0);

    void ClearTextCache();

    [[nodiscard]] SDL_Renderer* Handle() const { return m_renderer; }

private:
    struct CachedText {
        SDL_Texture* texture = nullptr;
        int w = 0;
        int h = 0;
    };

    const CachedText* AcquireText(const std::string& text, const std::string& fontPath, int size,
                                  Color color, int outline, int wrap);
    void Blit(SDL_Texture* texture, const Rect& dst, std::uint8_t alpha);

    SDL_Renderer* m_renderer;
    AssetCache& m_assets;
    int m_camX = 0;
    int m_camY = 0;
    int m_logicalW = 1280;
    int m_logicalH = 720;
    std::unordered_map<std::string, CachedText> m_textCache;
};

}
