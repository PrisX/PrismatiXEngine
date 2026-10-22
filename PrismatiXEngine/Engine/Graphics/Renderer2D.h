#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Engine/Core/Result.h"
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
enum class ImageBlendMode { Normal, Additive, Multiply };
enum class HorizontalAlignment { Left, Center, Right };
enum class VerticalAlignment { Top, Center, Bottom };

using ScreenEffectHandle = std::uint64_t;

// A compact, backend-neutral render plan. JavaScript registers one of these
// once; the Renderer executes the expensive per-frame work natively against
// the outgoing/incoming screen textures.
struct ScreenEffectDefinition {
    std::string id;
    // none, fade, crossfade, slide-left, slide-right, or tiles.
    std::string operation;
    int columns = 8;
    int rows = 6;
    float stagger = 0.35f;
    std::string order = "row-major";
    // Set only by the packaged .pxeffect loader. JavaScript cannot inject a
    // shader id or GPU resource binding through RegisterScreenEffect.
    std::string customEffect;
};

enum class ScreenEffectStatus : std::uint8_t {
    Unknown,
    Playing,
    Completed,
    Stopped,
    Cancelled,
};

struct ScreenEffectPlayback {
    ScreenEffectHandle handle = 0;
    std::string effect;
    float durationSeconds = 0.0f;
    float elapsedSeconds = 0.0f;
    bool paused = false;
    bool easeInOut = false;
    ScreenEffectStatus status = ScreenEffectStatus::Unknown;
    float randomSeed = 0.0f;
    std::array<std::array<float, 4>, 8> parameters{};
};

struct Shadow {
    bool enabled = false;
    int offsetX = 3;
    int offsetY = 3;
    std::uint8_t alpha = 110;
};

struct SpriteBatchItem {
    Rect source;
    Rect destination;
    Color color{255, 255, 255, 255};
    float rotation = 0.0f;
    bool sourceNormalized = false;
};

// Backend-neutral 2x3 affine transform.  It intentionally contains no native
// renderer handles so retained drawables can safely capture and replay it.
// Points are mapped as (a*x + c*y + tx, b*x + d*y + ty).
struct AffineTransform2D {
    float a = 1.0f;
    float b = 0.0f;
    float c = 0.0f;
    float d = 1.0f;
    float tx = 0.0f;
    float ty = 0.0f;
};

// Layer-1 extension drawing (@prismatix/runtime/raw). The same calls work on
// every SDL renderer the Player uses (Direct3D, Metal, OpenGL, the WebGL2 Web
// Player and SDL_GPU on WebGPU), because they reduce to SDL geometry,
// streaming textures and texture render targets.
struct RawVertex {
    float x = 0.0f;
    float y = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

enum class RawBlendMode : std::uint8_t {
    Normal,
    Premultiplied,
    Additive,
    Multiply,
    Replace,
};

class Renderer2D {
public:
    enum class TextSamplingMode { Auto, Fixed };
    Renderer2D(SDL_Renderer* renderer, AssetCache& assets,
               bool gpuEffects = false);
    ~Renderer2D();

    void SetLogicalSize(int width, int height, bool updateTextDensity = true);
    // Re-derives the text raster density after the window's pixel size changes.
    void UpdateTextDensity();
    // The backend lost render-target contents (Android/iOS resume, device
    // reset). Drops every offscreen target so the next frame recreates them
    // cleared; an in-flight screen effect or presentation hold ends early
    // rather than showing stale memory.
    void ResetRenderTargets();
    void GetLogicalSize(int& width, int& height) const;

    // Runtime owns these calls. Every complete frame is rendered into a
    // texture, making the last presented frame available as an immutable
    // outgoing snapshot when a transition starts mid-frame.
    [[nodiscard]] bool BeginFrame(Color clearColor);
    void EndFrame();
    // Snapshot the last presented frame while an incoming screen's images
    // preload. Unlike a paused transition, this preserves an authored "none"
    // transition: releasing the hold reveals the incoming frame immediately.
    [[nodiscard]] bool BeginPresentationHold();
    void EndPresentationHold() { m_presentationHoldActive = false; }
    [[nodiscard]] bool PresentationHoldActive() const {
        return m_presentationHoldActive;
    }

    Status RegisterScreenEffect(ScreenEffectDefinition definition);
    [[nodiscard]] bool HasScreenEffect(std::string_view id) const;
    [[nodiscard]] std::vector<std::string> ScreenEffectIds() const;
    // `easeInOut` shapes fade, crossfade and slide progress with smoothstep;
    // UI presentation transitions use it, authored story transitions stay
    // linear like Artemis/KiriKiri "trans".
    [[nodiscard]] ScreenEffectHandle PlayScreenEffect(
        std::string_view id, float durationSeconds,
        const CustomEffectNamedParameters* parameters = nullptr,
        bool easeInOut = false);
    // Dissolves the last presented frame into the frames that follow through
    // a grayscale rule image, the KiriKiri/Artemis whole-screen "trans".
    // `vague` is the soft edge width in rule levels (0-255). Works on every
    // graphics tier: the mask is applied with a blend mode, not a shader.
    [[nodiscard]] ScreenEffectHandle PlayRuleScreenTransition(
        const std::string& rulePath, float durationSeconds, int vague);
    [[nodiscard]] bool StopScreenEffect(ScreenEffectHandle handle);
    [[nodiscard]] bool CancelScreenEffect(ScreenEffectHandle handle);
    // Holds the outgoing snapshot while the incoming screen prepares assets;
    // unpausing resumes the same timeline without recapturing the frame.
    [[nodiscard]] bool PauseScreenEffect(ScreenEffectHandle handle, bool paused);
    [[nodiscard]] bool ScreenEffectPlaying(ScreenEffectHandle handle) const;
    [[nodiscard]] ScreenEffectStatus ScreenEffectState(
        ScreenEffectHandle handle) const;
    [[nodiscard]] std::optional<ScreenEffectPlayback> ActiveScreenEffect() const;
    void UpdateScreenEffects(float deltaSeconds);

    void SetCameraOffset(int x, int y) { m_camX = x, m_camY = y; }
    void ResetCamera() { m_camX = 0, m_camY = 0; }
    [[nodiscard]] Vec2 CameraOffset() const {
        return {static_cast<float>(m_camX), static_cast<float>(m_camY)};
    }

    void DrawRect(const Rect& rect, Color color);
    void DrawRoundedRect(const Rect& rect, float radius, Color color);
    void DrawBorder(const Rect& rect, float width, float radius, Color color);
    // A rounded rectangle whose colour fades from `from` (top or left) to
    // `to` (bottom or right).
    void DrawRoundedRectGradient(const Rect& rect, float radius, Color from, Color to,
                                 bool horizontal = false);
    // A stroke that follows the rounded corners, unlike DrawBorder.
    void DrawRoundedBorder(const Rect& rect, float width, float radius, Color color);
    // A blurred drop shadow approximated by stacked rounded rectangles.
    void DrawSoftShadow(const Rect& rect, float radius, float blur, Color color);
    void PushTransform(Vec2 pivot, Vec2 scale = {1,1}, float rotationDegrees = 0.0f,
                       Color modulate = {255,255,255,255});
    void PushAffineTransform(const AffineTransform2D& transform,
                             Color modulate = {255,255,255,255});
    void PopTransform();
    [[nodiscard]] Vec2 TransformPoint(Vec2 point) const;
    [[nodiscard]] Rect TransformBounds(Rect rect) const;
    void PushClip(const Rect& rect);
    void PopClip();

    void DrawImage(const std::string& path, const Rect& dst, std::uint8_t alpha = 255);
    // Blend mode is scoped to this draw, even when layers share one texture.
    void DrawImageBlended(const std::string& path, const Rect& dst,
                          std::uint8_t alpha, ImageBlendMode blendMode);
    void PreloadImage(const std::string& path) {
        m_assets.PreloadTexture(path, AssetPreloadPriority::Normal);
    }
    [[nodiscard]] bool ImageReady(const std::string& path) const {
        return m_assets.HasResidentTexture(path);
    }
    void DrawImageInRect(const std::string& path, const Rect& bounds, ContentScaleMode mode,
                         HorizontalAlignment horizontal = HorizontalAlignment::Center,
                         VerticalAlignment vertical = VerticalAlignment::Center,
                         std::uint8_t alpha = 255);
    // `margins` are in image pixels; the border is drawn `marginScale` times
    // that size (0.5 for an image drawn at twice the layout's resolution).
    void DrawNinePatch(const std::string& path, const Rect& bounds, Rect margins,
                       bool drawCenter = true, std::uint8_t alpha = 255,
                       float marginScale = 1.0f);
    // Draws a caller-owned backend-neutral texture (for example video or a
    // streaming transition mask).
    void DrawTexture(TextureHandle texture, const Rect& dst,
                     std::uint8_t alpha = 255);
    // Source rectangles use texture pixels. Invalid/empty regions are ignored
    // and valid regions are clipped to the texture, keeping callers away from
    // backend texture pointers and unchecked UVs.
    void DrawImageRegion(const std::string& path, const Rect& sourcePixels,
                         const Rect& dst, std::uint8_t alpha = 255);
    void DrawImageRegionBlended(const std::string& path, const Rect& sourcePixels,
                                const Rect& dst, std::uint8_t alpha,
                                ImageBlendMode blendMode);
    void DrawTextureRegion(TextureHandle texture, const Rect& sourcePixels,
                           const Rect& dst, std::uint8_t alpha = 255);
    void DrawSpriteBatch(const std::string& texture,
                         const std::vector<SpriteBatchItem>& items);
    [[nodiscard]] std::uint64_t GeometryBatchCount() const {
        return m_geometryBatchCount;
    }
    [[nodiscard]] std::uint64_t GeometryBatchItems() const {
        return m_geometryBatchItems;
    }
    Rect DrawImageAuto(const std::string& path, DisplayMode mode, std::uint8_t alpha = 255, int offsetX = 0, int offsetY = 0, float scale = 1.0f, Shadow shadow = {});

    void DrawText(const std::string& text, float x, float y, const std::string& fontPath, int size, Color color, std::uint8_t alpha = 255, int wrap = 0);
    // Centre puts the letters in the middle of `bounds`, not the line box:
    // the line moves by OpticalCenterOffset. `opticalReference` is the text
    // whose script picks the reference letter (a tracked label drawn one
    // character at a time passes its whole text); empty means `text`.
    void DrawTextInRect(const std::string& text, const Rect& bounds, const std::string& fontPath,
                        int size, Color color, HorizontalAlignment horizontal,
                        VerticalAlignment vertical, bool wrap, std::uint8_t alpha = 255,
                        int outlineSize = 0, Color outlineColor = {0, 0, 0, 255},
                        bool newlinesOnly = false, std::string_view opticalReference = {});
    // How far a vertically centred line of `text` moves (logical pixels,
    // negative is up) so that the ink of a reference letter, rather than the
    // font's ascent-plus-descent line box, sits on the middle: the ideograph
    // 中 when the text has CJK, kana or Hangul (its face fills the em box the
    // way the other ideographs do), else the capital H (cap height, as CSS
    // text-box-trim and most UI toolkits centre Latin). Fonts with a deep
    // descent otherwise draw every centred caption visibly low. Per font,
    // size and reference; cached.
    [[nodiscard]] float OpticalCenterOffset(std::string_view text, const std::string& fontPath,
                                            int size);
    void DrawTextOutline(const std::string& text, float x, float y, const std::string& fontPath, int size, Color textColor, Color outlineColor, int outlineSize, std::uint8_t alpha = 255, bool shadow = false, int wrap = 0);
    [[nodiscard]] Vec2 MeasureText(const std::string& text, const std::string& fontPath, int size, int wrap = 0);
    [[nodiscard]] text::TextLayout LayoutText(
        const std::string& text, const std::string& fontPath, int size,
        int wrap = 0,
        text::TextOrientation orientation = text::TextOrientation::Horizontal,
        std::size_t verticalRows = 0) const;
    [[nodiscard]] text::TextLayout LayoutTextRuns(
        const std::string& text, const std::vector<text::TextFontRun>& runs,
        int wrap = 0) const {
        return m_textLayout.LayoutRuns(text, runs, wrap);
    }
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
    // GPU rule transition: draws until EndRuleMask fade against the rule
    // image at `rulePath`. False when unsupported or the rule is not resident.
    [[nodiscard]] bool SupportsRuleMask() const { return m_compositor.RuleMaskSupported(); }
    [[nodiscard]] bool BeginRuleMask(const std::string& rulePath, float threshold,
                                     float vague);
    void EndRuleMask() { m_compositor.EndRuleMask(); }
    void ReleaseRuleMask() { m_compositor.ReleaseRuleMask(); }
    bool LoadCustomEffects(const std::vector<CustomEffectDescriptor>& effects,
                           const io::VFS& vfs);
    [[nodiscard]] bool HasCustomEffect(
        std::string_view id, std::string_view target = {}) const {
        return m_compositor.HasCustomEffect(id, target);
    }
    [[nodiscard]] std::string CustomEffectTarget(std::string_view id) const {
        return m_compositor.CustomEffectTarget(id);
    }
    [[nodiscard]] std::vector<std::string> CustomEffectIds() const {
        return m_compositor.CustomEffectIds();
    }
    [[nodiscard]] std::optional<std::array<std::array<float, 4>, 8>>
    CustomEffectDefaults(std::string_view id) const {
        return m_compositor.CustomEffectDefaults(id);
    }
    [[nodiscard]] std::optional<std::array<std::array<float, 4>, 8>>
    ResolveCustomEffectParameters(
        std::string_view id,
        const CustomEffectNamedParameters& parameters) const {
        return m_compositor.ResolveCustomEffectParameters(id, parameters);
    }
    [[nodiscard]] bool BeginNodeEffect(
        std::string_view id, float progress, float randomSeed,
        const std::array<std::array<float, 4>, 8>& parameters,
        const Rect& viewport) {
        return m_compositor.BeginNodeEffect(
            id, progress, randomSeed, parameters,
            {viewport.x, viewport.y, viewport.w, viewport.h});
    }
    void EndNodeEffect() { m_compositor.EndNodeEffect(); }

    // Captures subsequent draws into a transparent logical-size target. The
    // matching EndComposition composites that target once, so a node shader
    // observes an entire group rather than being re-run for each leaf.
    // Calls may be nested. A false return leaves renderer state unchanged.
    [[nodiscard]] bool BeginComposition();
    void EndComposition(
        std::string_view effect, float progress, float randomSeed,
        const std::array<std::array<float, 4>, 8>& parameters,
        const Rect& viewport);

    // Reuses two logical-size render targets for image-layer alpha masks.
    // The caller draws content, switches to coverage, draws the mask image,
    // then ends. Failed setup leaves state unchanged; failed coverage must be
    // ended with composite=false so an unmasked layer is never exposed.
    [[nodiscard]] bool BeginAlphaMask();
    [[nodiscard]] bool BeginAlphaMaskCoverage();
    void EndAlphaMask(bool composite = true);

    [[nodiscard]] SDL_Renderer* Handle() const { return m_renderer; }

    // Raw extension resources. A writable texture is RGBA8 (rows top to
    // bottom); a render target starts transparent. Both are empty when the
    // backend cannot create them.
    [[nodiscard]] TextureResource CreateRawTexture(int width, int height,
                                                   bool linear);
    [[nodiscard]] TextureResource CreateRawTarget(int width, int height,
                                                  bool linear);
    // Replaces `region` (texture pixels) with `pixels`, `pitchBytes` per row.
    [[nodiscard]] bool UpdateRawTexture(TextureResource& texture,
                                        const Rect& region,
                                        const std::uint8_t* pixels,
                                        std::size_t pitchBytes);
    // Redirects drawing into `target` until EndRawTarget, with an identity
    // transform and no clip; `clear` fills it first. Not nestable.
    [[nodiscard]] bool BeginRawTarget(TextureHandle target,
                                      std::optional<Color> clear);
    void EndRawTarget();
    // Draws a triangle list (`indices` may be null) with `texture` (or vertex
    // colors only), `transform` applied to the positions. On the screen the
    // renderer's own transform and modulation apply too.
    void DrawRawGeometry(TextureHandle texture, const RawVertex* vertices,
                         int vertexCount, const int* indices, int indexCount,
                         RawBlendMode blend,
                         const AffineTransform2D& transform);
    [[nodiscard]] std::string BackendName() const;
    // A packaged image as a texture (loaded on first use), or empty.
    [[nodiscard]] TextureHandle AssetTexture(const std::string& path) {
        return m_assets.Texture(path);
    }

private:
    bool RenderGeometry(SDL_Texture* texture, const SDL_Vertex* vertices,
                         int count, const int* indices, int indexCount);
    struct TransformState {
        float a=1,b=0,c=0,d=1,tx=0,ty=0;
        Color modulate{255,255,255,255};
    };
    [[nodiscard]] Color TransformColor(Color color) const;

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
    void BlitUntransformed(SDL_Texture* texture);
    [[nodiscard]] bool ClearMaskTarget(SDL_Texture* texture);
    void DestroyMaskTargets();
    [[nodiscard]] bool EnsureFrameTargets();
    void DestroyFrameTargets();
    void PresentScreenFrame();
    void RenderScreenEffect(const ScreenEffectDefinition& definition,
                            float progress);
    void FinishScreenEffect(ScreenEffectStatus status);
    void RenderRuleScreenTransition(float progress);
    void DestroyRuleScreenTargets();

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
    std::unordered_map<std::string, float> m_opticalOffsets;
    std::vector<Rect> m_clipStack;
    std::vector<TransformState> m_transformStack{{}};
    struct CompositionState {
        SDL_Texture* texture = nullptr;
        SDL_Texture* previousTarget = nullptr;
    };
    std::vector<CompositionState> m_compositionStack;
    SDL_Texture* m_maskContentTarget = nullptr;
    SDL_Texture* m_maskCoverageTarget = nullptr;
    SDL_Texture* m_maskPreviousTarget = nullptr;
    bool m_maskActive = false;
    bool m_maskCoverageActive = false;
    SDL_Texture* m_frameTarget = nullptr;
    SDL_Texture* m_presentedTarget = nullptr;
    SDL_Texture* m_outgoingTarget = nullptr;
    SDL_Texture* m_presentationHoldTarget = nullptr;
    SDL_Texture* m_framePreviousTarget = nullptr;
    std::unordered_map<std::string, ScreenEffectDefinition> m_screenEffectDefinitions;
    std::unordered_map<ScreenEffectHandle, ScreenEffectStatus> m_screenEffectStates;
    std::optional<ScreenEffectPlayback> m_activeScreenEffect;
    ScreenEffectHandle m_nextScreenEffectHandle = 1;
    struct RuleScreenTransition {
        std::string path;
        int vague = 64;
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> values;
        std::vector<std::uint8_t> rowMin;
        std::vector<std::uint8_t> rowMax;
        std::vector<std::uint8_t> maskPixels;
        std::array<std::uint8_t, 256> alphaByRule{};
        bool uploaded = false;
    };
    std::optional<RuleScreenTransition> m_ruleScreen;
    SDL_Texture* m_ruleScreenMask = nullptr;
    SDL_Texture* m_ruleScreenMasked = nullptr;
    bool m_frameRecording = false;
    bool m_presentationHoldActive = false;
    std::uint64_t m_geometryBatchCount = 0;
    std::uint64_t m_geometryBatchItems = 0;
    struct RawTargetState {
        SDL_Texture* previousTarget = nullptr;
        std::vector<TransformState> transforms;
        std::vector<Rect> clips;
    };
    std::optional<RawTargetState> m_rawTarget;
    // The camera offset while drawing into a raw target (always zero).
    std::pair<int, int> m_rawTargetCamera{0, 0};
};

}  // namespace px::graphics
