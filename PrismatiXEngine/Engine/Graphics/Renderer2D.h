#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "Engine/Core/Types.h"
#include "Engine/Graphics/AssetCache.h"

struct SDL_Renderer;
struct SDL_Texture;
struct TTF_Text;
struct TTF_TextEngine;

namespace px::graphics {

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
    ~Renderer2D();

    void SetLogicalSize(int width, int height);
    void GetLogicalSize(int& width, int& height) const;

    void SetCameraOffset(int x, int y) { m_camX = x, m_camY = y; }
    void ResetCamera() { m_camX = 0, m_camY = 0; }

    void DrawRect(const Rect& rect, Color color);
    void DrawRoundedRect(const Rect& rect, float radius, Color color);
    void PushClip(const Rect& rect);
    void PopClip();

    void DrawImage(const std::string& path, const Rect& dst, std::uint8_t alpha = 255);
    // Draws a caller-owned texture (e.g. a streaming transition mask).
    void DrawTexture(SDL_Texture* texture, const Rect& dst, std::uint8_t alpha = 255);
    Rect DrawImageAuto(const std::string& path, DisplayMode mode, std::uint8_t alpha = 255, int offsetX = 0, int offsetY = 0, float scale = 1.0f, Shadow shadow = {});

    void DrawText(const std::string& text, float x, float y, const std::string& fontPath, int size, Color color, std::uint8_t alpha = 255, int wrap = 0);
    void DrawTextOutline(const std::string& text, float x, float y, const std::string& fontPath, int size, Color textColor, Color outlineColor, int outlineSize, std::uint8_t alpha = 255, bool shadow = false, int wrap = 0);
    [[nodiscard]] Vec2 MeasureText(const std::string& text, const std::string& fontPath, int size, int wrap = 0);

    void SetTextSupersample(int factor);
    [[nodiscard]] int TextSupersample() const { return m_textSupersample; }

    void ClearTextCache();

    [[nodiscard]] SDL_Renderer* Handle() const { return m_renderer; }

private:
    struct CachedText {
        TTF_Text* text = nullptr;
        int w = 0;
        int h = 0;
    };

    const CachedText* AcquireText(const std::string& text, const std::string& fontPath, int size, Color color, int outline, int wrap);
    void Blit(SDL_Texture* texture, const Rect& dst, std::uint8_t alpha);

    SDL_Renderer* m_renderer;
    TTF_TextEngine* m_textEngine = nullptr;
    AssetCache& m_assets;
    int m_camX = 0;
    int m_camY = 0;
    int m_logicalW = 1280;
    int m_logicalH = 720;
    int m_textSupersample = 2;
    std::unordered_map<std::string, CachedText> m_textCache;
    std::vector<Rect> m_clipStack;
};

}  // namespace px::graphics
