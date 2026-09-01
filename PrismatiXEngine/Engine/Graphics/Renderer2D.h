#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "Engine/Core/Types.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Compositor2D.h"
#include "Engine/Text/TextLayout.h"

struct SDL_Renderer;
struct SDL_Texture;

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

enum class ContentScaleMode { Stretch, Fit, Fill, Original };
enum class HorizontalAlignment { Left, Center, Right };
enum class VerticalAlignment { Top, Center, Bottom };

struct Shadow {
    bool enabled = false;
    int offsetX = 3;
    int offsetY = 3;
    std::uint8_t alpha = 110;
};

class Renderer2D {
public:
    enum class TextSamplingMode { Auto, Fixed };
    Renderer2D(SDL_Renderer* renderer, AssetCache& assets,
               bool gpuEffects = false);
    ~Renderer2D();

    void SetLogicalSize(int width, int height, bool updateTextDensity = true);
    void GetLogicalSize(int& width, int& height) const;

    void SetCameraOffset(int x, int y) { m_camX = x, m_camY = y; }
    void ResetCamera() { m_camX = 0, m_camY = 0; }
    [[nodiscard]] Vec2 CameraOffset() const {
        return {static_cast<float>(m_camX), static_cast<float>(m_camY)};
    }

    void DrawRect(const Rect& rect, Color color);
    void DrawRoundedRect(const Rect& rect, float radius, Color color);
    void DrawBorder(const Rect& rect, float width, float radius, Color color);
    void PushTransform(Vec2 pivot, Vec2 scale = {1,1}, float rotationDegrees = 0.0f,
                       Color modulate = {255,255,255,255});
    void PopTransform();
    void PushClip(const Rect& rect);
    void PopClip();

    void DrawImage(const std::string& path, const Rect& dst, std::uint8_t alpha = 255);
    void DrawImageInRect(const std::string& path, const Rect& bounds, ContentScaleMode mode,
                         HorizontalAlignment horizontal = HorizontalAlignment::Center,
                         VerticalAlignment vertical = VerticalAlignment::Center,
                         std::uint8_t alpha = 255);
    void DrawNinePatch(const std::string& path, const Rect& bounds, Rect margins,
                       bool drawCenter = true, std::uint8_t alpha = 255);
    // Draws a caller-owned backend-neutral texture (for example video or a
    // streaming transition mask).
    void DrawTexture(TextureHandle texture, const Rect& dst,
                     std::uint8_t alpha = 255);
    Rect DrawImageAuto(const std::string& path, DisplayMode mode, std::uint8_t alpha = 255, int offsetX = 0, int offsetY = 0, float scale = 1.0f, Shadow shadow = {});

    void DrawText(const std::string& text, float x, float y, const std::string& fontPath, int size, Color color, std::uint8_t alpha = 255, int wrap = 0);
    void DrawTextInRect(const std::string& text, const Rect& bounds, const std::string& fontPath,
                        int size, Color color, HorizontalAlignment horizontal,
                        VerticalAlignment vertical, bool wrap, std::uint8_t alpha = 255);
    void DrawTextOutline(const std::string& text, float x, float y, const std::string& fontPath, int size, Color textColor, Color outlineColor, int outlineSize, std::uint8_t alpha = 255, bool shadow = false, int wrap = 0);
    [[nodiscard]] Vec2 MeasureText(const std::string& text, const std::string& fontPath, int size, int wrap = 0);
    [[nodiscard]] text::TextLayout LayoutText(
        const std::string& text, const std::string& fontPath, int size,
        int wrap = 0,
        text::TextOrientation orientation = text::TextOrientation::Horizontal,
        std::size_t verticalRows = 0) const;
    void DrawTextLayout(const text::TextLayout& layout, Vec2 origin,
                        const std::string& fontPath, int size, Color color,
                        std::uint8_t alpha = 255);
    void SetTextLocale(std::string locale,
                       std::vector<std::string> fontFallbackChain = {});
    [[nodiscard]] const std::string& TextLocale() const { return m_textLayout.Locale(); }

    void SetTextSupersample(int factor);
    void SetTextSupersampleAuto();
    void SetPreviewContext(float displayPixelsPerLogical, bool clarityCompensation);
    [[nodiscard]] int TextSupersample() const { return m_effectiveTextSupersample; }
    [[nodiscard]] TextSamplingMode TextSampling() const { return m_textSamplingMode; }

    void ClearTextCache();

    [[nodiscard]] bool BeginStageLayer() { return m_compositor.BeginStage(); }
    void EndStageLayer(const StagePostEffects& effects) {
        m_compositor.EndStage(effects);
    }
    [[nodiscard]] bool SupportsStagePostEffects() const {
        return m_compositor.Enabled();
    }
    [[nodiscard]] bool Ready() const { return m_compositor.Ready(); }
    bool LoadCustomEffects(const std::vector<CustomEffectDescriptor>& effects,
                           const io::VFS& vfs) {
        return m_compositor.LoadCustomEffects(effects, vfs);
    }
    [[nodiscard]] bool HasCustomEffect(std::string_view id) const {
        return m_compositor.HasCustomEffect(id);
    }
    [[nodiscard]] std::vector<std::string> CustomEffectIds() const {
        return m_compositor.CustomEffectIds();
    }
    [[nodiscard]] std::optional<std::array<std::array<float, 4>, 8>>
    CustomEffectDefaults(std::string_view id) const {
        return m_compositor.CustomEffectDefaults(id);
    }

    [[nodiscard]] SDL_Renderer* Handle() const { return m_renderer; }

private:
    struct TransformState {
        float a=1,b=0,c=0,d=1,tx=0,ty=0;
        Color modulate{255,255,255,255};
    };
    [[nodiscard]] Vec2 TransformPoint(Vec2 point) const;
    [[nodiscard]] Color TransformColor(Color color) const;
    [[nodiscard]] Rect TransformBounds(Rect rect) const;

    struct CachedText {
        SDL_Texture* texture = nullptr;
        int w = 0;
        int h = 0;
    };

    const CachedText* AcquireText(const std::string& text, const std::string& fontPath, int size,
                                  Color color, int outline, int wrap,
                                  HorizontalAlignment alignment = HorizontalAlignment::Left,
                                  text::TextOrientation orientation =
                                      text::TextOrientation::Horizontal);
    void Blit(SDL_Texture* texture, const Rect& dst, std::uint8_t alpha);
    void BlitRegion(SDL_Texture* texture, const Rect& sourcePixels, const Rect& dst,
                    std::uint8_t alpha);

    SDL_Renderer* m_renderer;
    AssetCache& m_assets;
    text::TextLayoutService m_textLayout;
    Compositor2D m_compositor;
    int m_camX = 0;
    int m_camY = 0;
    int m_logicalW = 1280;
    int m_logicalH = 720;
    TextSamplingMode m_textSamplingMode = TextSamplingMode::Auto;
    int m_fixedTextSupersample = 2;
    int m_effectiveTextSupersample = 2;
    float m_displayPixelsPerLogical = 1.0f;
    bool m_clarityCompensation = false;
    std::unordered_map<std::string, CachedText> m_textCache;
    std::vector<Rect> m_clipStack;
    std::vector<TransformState> m_transformStack{{}};
};

}  // namespace px::graphics
