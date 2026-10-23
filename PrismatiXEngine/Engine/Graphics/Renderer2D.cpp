#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Diagnostics/FrameProfiler.h"

#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Support/Logger.h"
#include "Engine/Text/Typography.h"

#include "Engine/Platform/Web/WebEffects.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace px::graphics {

namespace {
#if defined(PRISMATIX_WEB_GL_EFFECTS)
constexpr auto kTargetFormat = SDL_PIXELFORMAT_RGBA32;
#else
constexpr auto kTargetFormat = SDL_PIXELFORMAT_RGBA8888;
#endif
constexpr std::size_t kTextCacheLimit = 600;

SDL_Texture* NativeTexture(const TextureHandle texture) {
    return static_cast<SDL_Texture*>(
        texture.Native(TextureBackend::SdlRenderer));
}

SDL_BlendMode NativeBlendMode(const ImageBlendMode mode) {
    switch (mode) {
    case ImageBlendMode::Additive: return SDL_BLENDMODE_ADD;
    case ImageBlendMode::Multiply: return SDL_BLENDMODE_MUL;
    default: return SDL_BLENDMODE_BLEND;
    }
}

SDL_BlendMode AlphaCoverageBlendMode() {
    // The mask's alpha multiplies both existing premultiplied color and alpha.
    return SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
}

SDL_BlendMode InvertedCoverageBlendMode() {
    // One minus the mask's alpha multiplies color and alpha: the content
    // stays where the mask is not.
    return SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
}

SDL_BlendMode PremultipliedBlendMode() {
    // A render target drawn with normal alpha blending contains premultiplied
    // color, so compositing it with SDL_BLENDMODE_BLEND would square alpha.
    return SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE, SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA, SDL_BLENDOPERATION_ADD);
}

SDL_FColor ToFColor(Color c, float alphaScale = 1.0f) { return SDL_FColor{ c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, (c.a / 255.0f) * alphaScale }; }

void AppendArc(std::vector<SDL_Vertex>& out, float cx, float cy, float radius, float a0, float a1, SDL_FColor color, int segments) {
    for (int i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(std::max(segments, 1));
        const float a = a0 + (a1 - a0) * t;
        SDL_Vertex v{};
        v.position = SDL_FPoint{ cx + std::cos(a) * radius, cy + std::sin(a) * radius };
        v.color = color;
        out.push_back(v);
    }
}

// The outline of a rounded rectangle, clockwise from the top-right corner.
// Corners get more segments as they grow so large radii stay smooth.
std::vector<SDL_FPoint> RoundedOutline(const Rect& rect, float radius) {
    constexpr float kPi = 3.14159265358979323846f;
    const float r = std::clamp(radius, 0.0f, std::min(rect.w, rect.h) * 0.5f);
    const int segments = std::clamp(static_cast<int>(r * 0.5f), 6, 32);
    const float left = rect.x, top = rect.y, right = rect.x + rect.w, bottom = rect.y + rect.h;
    std::vector<SDL_FPoint> out;
    out.reserve(4 * (segments + 1));
    const auto arc = [&](float cx, float cy, float a0, float a1) {
        for (int i = 0; i <= segments; ++i) {
            const float a = a0 + (a1 - a0) * static_cast<float>(i) / static_cast<float>(segments);
            out.push_back({cx + std::cos(a) * r, cy + std::sin(a) * r});
        }
    };
    arc(right - r, top + r, -kPi * 0.5f, 0.0f);
    arc(right - r, bottom - r, 0.0f, kPi * 0.5f);
    arc(left + r, bottom - r, kPi * 0.5f, kPi);
    arc(left + r, top + r, kPi, kPi * 1.5f);
    return out;
}

SDL_FColor LerpColor(SDL_FColor a, SDL_FColor b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
            a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

std::string MakeKey(const std::string& text, const std::string& font, int size, Color c, int outline, int wrap, int ss, int alignment, const std::string& locale) {
    return text + "\x01" + font + "\x01" + std::to_string(size) + "\x01" + std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + "," + std::to_string(c.a) + "\x01" + std::to_string(outline) + "\x01" + std::to_string(wrap) + "\x01" +
           std::to_string(ss)+"\x01"+std::to_string(alignment)+"\x01"+locale;
}
}  // namespace

bool Renderer2D::RenderGeometry(SDL_Texture* texture, const SDL_Vertex* vertices,
                                const int count, const int* indices, const int indexCount) {
#if defined(PRISMATIX_WEB_GL_EFFECTS)
    if (m_compositor.DrawWebGeometry(texture, vertices, count, indices, indexCount)) return true;
#endif
    return SDL_RenderGeometry(m_renderer, texture, vertices, count, indices, indexCount);
}

Renderer2D::Renderer2D(SDL_Renderer* renderer, AssetCache& assets,
                       const bool gpuEffects)
    : m_renderer(renderer), m_assets(assets), m_textLayout(assets),
      m_compositor(renderer, gpuEffects) {
    for (const char* operation : {"none", "fade", "crossfade", "slide-left",
                                  "slide-right", "rule"}) {
        ScreenEffectDefinition definition;
        definition.id = operation;
        definition.operation = operation;
        m_screenEffectDefinitions.emplace(definition.id, std::move(definition));
    }
}

Renderer2D::~Renderer2D() {
    DestroyMaskTargets();
    while (!m_compositionStack.empty()) {
        const CompositionState composition = m_compositionStack.back();
        m_compositionStack.pop_back();
        if (m_renderer)
            (void)SDL_SetRenderTarget(m_renderer, composition.previousTarget);
        if (composition.texture) SDL_DestroyTexture(composition.texture);
    }
    ClearTextCache();
    DestroyFrameTargets();
}

void Renderer2D::SetLogicalSize(int width, int height, bool updateTextDensity) {
    DestroyMaskTargets();
    m_logicalW = width;
    m_logicalH = height;
    SDL_SetRenderLogicalPresentation(m_renderer, width, height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    m_compositor.SetLogicalSize(width, height);
    DestroyFrameTargets();
    if(updateTextDensity)UpdateTextDensity();
}

void Renderer2D::UpdateTextDensity() {
    if (m_textSamplingMode != TextSamplingMode::Auto || m_logicalW <= 0 || m_logicalH <= 0)
        return;
    // Glyphs are rasterized at the output resolution: 1x when the window is at
    // or below the logical size, 2x and up on high-density or 4K output.
    int outputW = m_logicalW, outputH = m_logicalH;
    (void)SDL_GetCurrentRenderOutputSize(m_renderer, &outputW, &outputH);
    const float density = std::min(static_cast<float>(outputW) / m_logicalW,
                                   static_cast<float>(outputH) / m_logicalH);
    const int effective = std::clamp(static_cast<int>(std::ceil(density - 0.05f)), 1, 4);
    if (effective != m_effectiveTextSupersample) {
        m_effectiveTextSupersample = effective;
        ClearTextCache();
    }
}

void Renderer2D::GetLogicalSize(int& width, int& height) const {
    width = m_logicalW;
    height = m_logicalH;
}

void Renderer2D::DrawRect(const Rect& rect, Color color) {
    color=TransformColor(color);const auto p0=TransformPoint({rect.x,rect.y}),p1=TransformPoint({rect.x+rect.w,rect.y}),p2=TransformPoint({rect.x+rect.w,rect.y+rect.h}),p3=TransformPoint({rect.x,rect.y+rect.h});const SDL_FColor fc=ToFColor(color);SDL_Vertex vertices[4]{{{p0.x,p0.y},fc,{}},{{p1.x,p1.y},fc,{}},{{p2.x,p2.y},fc,{}},{{p3.x,p3.y},fc,{}}};constexpr int indices[6]{0,1,2,0,2,3};SDL_SetRenderDrawBlendMode(m_renderer,SDL_BLENDMODE_BLEND);RenderGeometry(nullptr,vertices,4,indices,6);
}

void Renderer2D::DrawRoundedRect(const Rect& rect, float radius, Color color) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }
    const float r = std::clamp(radius, 0.0f, std::min(rect.w, rect.h) * 0.5f);
    if (r <= 0.5f) {
        DrawRect(rect, color);
        return;
    }

    const SDL_FColor fc = ToFColor(TransformColor(color));
    const float left = rect.x;
    const float top = rect.y;
    const float right = left + rect.w;
    const float bottom = top + rect.h;
    constexpr float kPi = 3.14159265358979323846f;
    constexpr int kSeg = 6;

    std::vector<SDL_Vertex> ring;
    ring.reserve(4 * (kSeg + 1));
    AppendArc(ring, right - r, top + r, r, -kPi * 0.5f, 0.0f, fc, kSeg);
    AppendArc(ring, right - r, bottom - r, r, 0.0f, kPi * 0.5f, fc, kSeg);
    AppendArc(ring, left + r, bottom - r, r, kPi * 0.5f, kPi, fc, kSeg);
    AppendArc(ring, left + r, top + r, r, kPi, kPi * 1.5f, fc, kSeg);

    SDL_Vertex center{};
    center.position = SDL_FPoint{ left + rect.w * 0.5f, top + rect.h * 0.5f };
    center.color = fc;

    std::vector<SDL_Vertex> fan;
    fan.reserve(ring.size() * 3);
    for (std::size_t i = 0; i < ring.size(); ++i) {
        fan.push_back(center);
        fan.push_back(ring[i]);
        fan.push_back(ring[(i + 1) % ring.size()]);
    }

    for(auto& vertex:fan){const auto transformed=TransformPoint({vertex.position.x,vertex.position.y});vertex.position={transformed.x,transformed.y};}
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    RenderGeometry(nullptr, fan.data(), static_cast<int>(fan.size()), nullptr, 0);
}

void Renderer2D::DrawRoundedRectGradient(const Rect& rect, float radius, Color from,
                                         Color to, bool horizontal) {
    if (rect.w <= 0.0f || rect.h <= 0.0f) return;
    const SDL_FColor a = ToFColor(TransformColor(from));
    const SDL_FColor b = ToFColor(TransformColor(to));
    const auto colorAt = [&](SDL_FPoint p) {
        return LerpColor(a, b, horizontal ? (p.x - rect.x) / rect.w : (p.y - rect.y) / rect.h);
    };
    // A fan from the centre reproduces a linear gradient exactly, because
    // the colour is a linear function of position.
    const auto ring = RoundedOutline(rect, radius);
    const SDL_FPoint centre{rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f};
    std::vector<SDL_Vertex> fan;
    fan.reserve(ring.size() * 3);
    const auto vertex = [&](SDL_FPoint p) {
        const auto t = TransformPoint({p.x, p.y});
        return SDL_Vertex{{t.x, t.y}, colorAt(p), {}};
    };
    for (std::size_t i = 0; i < ring.size(); ++i) {
        fan.push_back(vertex(centre));
        fan.push_back(vertex(ring[i]));
        fan.push_back(vertex(ring[(i + 1) % ring.size()]));
    }
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    RenderGeometry(nullptr, fan.data(), static_cast<int>(fan.size()), nullptr, 0);
}

void Renderer2D::DrawRoundedBorder(const Rect& rect, float width, float radius,
                                   Color color) {
    if (rect.w <= 0.0f || rect.h <= 0.0f || width <= 0.0f) return;
    const float r = std::clamp(radius, 0.0f, std::min(rect.w, rect.h) * 0.5f);
    const float stroke = std::min(width, std::min(rect.w, rect.h) * 0.5f);
    // Both outlines use the outer radius's segment count so their points pair up.
    auto outer = RoundedOutline(rect, r);
    const Rect inset{rect.x + stroke, rect.y + stroke,
                     std::max(0.0f, rect.w - stroke * 2), std::max(0.0f, rect.h - stroke * 2)};
    std::vector<SDL_FPoint> inner;
    inner.reserve(outer.size());
    const float innerRadius = std::max(0.0f, r - stroke);
    // Shrink each outer point toward its corner centre instead of rebuilding,
    // so the inner outline has exactly as many points.
    const int perCorner = static_cast<int>(outer.size() / 4);
    const float cr = std::clamp(r, 0.0f, std::min(rect.w, rect.h) * 0.5f);
    const SDL_FPoint centres[4]{{rect.x + rect.w - cr, rect.y + cr},
                                {rect.x + rect.w - cr, rect.y + rect.h - cr},
                                {rect.x + cr, rect.y + rect.h - cr},
                                {rect.x + cr, rect.y + cr}};
    const SDL_FPoint innerCentres[4]{
        {inset.x + inset.w - innerRadius, inset.y + innerRadius},
        {inset.x + inset.w - innerRadius, inset.y + inset.h - innerRadius},
        {inset.x + innerRadius, inset.y + inset.h - innerRadius},
        {inset.x + innerRadius, inset.y + innerRadius}};
    for (std::size_t i = 0; i < outer.size(); ++i) {
        const int corner = std::min(3, static_cast<int>(i) / std::max(1, perCorner));
        const SDL_FPoint c = centres[corner];
        const float dx = outer[i].x - c.x, dy = outer[i].y - c.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        const float ux = length > 0.0f ? dx / length : 0.0f;
        const float uy = length > 0.0f ? dy / length : 0.0f;
        inner.push_back({innerCentres[corner].x + ux * innerRadius,
                         innerCentres[corner].y + uy * innerRadius});
    }
    const SDL_FColor fc = ToFColor(TransformColor(color));
    std::vector<SDL_Vertex> strip;
    strip.reserve(outer.size() * 6);
    const auto vertex = [&](SDL_FPoint p) {
        const auto t = TransformPoint({p.x, p.y});
        return SDL_Vertex{{t.x, t.y}, fc, {}};
    };
    for (std::size_t i = 0; i < outer.size(); ++i) {
        const std::size_t j = (i + 1) % outer.size();
        strip.push_back(vertex(outer[i])); strip.push_back(vertex(outer[j])); strip.push_back(vertex(inner[i]));
        strip.push_back(vertex(inner[i])); strip.push_back(vertex(outer[j])); strip.push_back(vertex(inner[j]));
    }
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    RenderGeometry(nullptr, strip.data(), static_cast<int>(strip.size()), nullptr, 0);
}

void Renderer2D::DrawSoftShadow(const Rect& rect, float radius, float blur, Color color) {
    if (rect.w <= 0.0f || rect.h <= 0.0f || color.a == 0) return;
    if (blur <= 0.5f) { DrawRoundedRect(rect, radius, color); return; }
    // Stacked translucent rounded rectangles, widest and faintest first: a
    // cheap blur that needs no offscreen pass and works on every tier.
    const int layers = std::clamp(static_cast<int>(blur / 2.0f), 3, 12);
    const float total = static_cast<float>(color.a);
    for (int i = layers; i >= 1; --i) {
        const float spread = blur * static_cast<float>(i) / static_cast<float>(layers);
        Color layer = color;
        layer.a = static_cast<std::uint8_t>(std::clamp(total / static_cast<float>(layers) * 1.6f, 1.0f, 255.0f));
        DrawRoundedRect({rect.x - spread * 0.5f, rect.y - spread * 0.5f,
                         rect.w + spread, rect.h + spread},
                        radius + spread * 0.5f, layer);
    }
}

Status Renderer2D::RegisterScreenEffect(ScreenEffectDefinition definition) {
    const bool validId = !definition.id.empty() && definition.id.size() <= 128 &&
                         std::ranges::all_of(definition.id, [](const unsigned char c) {
                             return std::isalnum(c) || c == '.' || c == '_' || c == '-';
                         });
    const bool validOperation =
        definition.operation == "none" || definition.operation == "fade" ||
        definition.operation == "crossfade" ||
        definition.operation == "slide-left" ||
        definition.operation == "slide-right" || definition.operation == "tiles" ||
        (definition.operation == "custom" &&
         m_compositor.HasCustomEffect(definition.customEffect, "transition"));
    if (!validId || !validOperation || definition.columns < 1 ||
        definition.columns > 64 || definition.rows < 1 || definition.rows > 64 ||
        !std::isfinite(definition.stagger) || definition.stagger < 0.0f ||
        definition.stagger > 1.0f ||
        (definition.order != "row-major" && definition.order != "column-major" &&
         definition.order != "reverse")) {
        return Status::Fail(diag::Diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXGFX7610",
            .category = "Graphics.ScreenEffect",
            .message = "Screen effect definition is invalid",
            .details = definition.id});
    }
    m_screenEffectDefinitions.insert_or_assign(definition.id,
                                                std::move(definition));
    return Status::Ok();
}

bool Renderer2D::LoadCustomEffects(
    const std::vector<CustomEffectDescriptor>& effects, const io::VFS& vfs) {
    for (const auto& effect : effects) {
        if (effect.targetLayer != "transition") continue;
        const auto existing = m_screenEffectDefinitions.find(effect.id);
        if (existing != m_screenEffectDefinitions.end() &&
            existing->second.operation != "custom")
            return false;
    }
    if (!m_compositor.LoadCustomEffects(effects, vfs)) return false;
    std::erase_if(m_screenEffectDefinitions, [](const auto& entry) {
        return entry.second.operation == "custom";
    });
    for (const auto& id : m_compositor.CustomEffectIds()) {
        if (m_compositor.CustomEffectTarget(id) != "transition") continue;
        if (const auto existing = m_screenEffectDefinitions.find(id);
            existing != m_screenEffectDefinitions.end())
            return false;
        ScreenEffectDefinition definition;
        definition.id = id;
        definition.operation = "custom";
        definition.customEffect = id;
        m_screenEffectDefinitions.emplace(id, std::move(definition));
    }
    return true;
}

bool Renderer2D::HasScreenEffect(const std::string_view id) const {
    return m_screenEffectDefinitions.contains(std::string(id));
}

std::vector<std::string> Renderer2D::ScreenEffectIds() const {
    std::vector<std::string> result;
    result.reserve(m_screenEffectDefinitions.size());
    for (const auto& [id, _] : m_screenEffectDefinitions) result.push_back(id);
    std::ranges::sort(result);
    return result;
}

bool Renderer2D::EnsureFrameTargets() {
    if (!m_renderer || m_logicalW <= 0 || m_logicalH <= 0) return false;
    if (m_frameTarget && m_presentedTarget && m_outgoingTarget) return true;
    const auto create = [this]() {
        SDL_Texture* texture = SDL_CreateTexture(
            m_renderer, kTargetFormat, SDL_TEXTUREACCESS_TARGET,
            m_logicalW, m_logicalH);
        if (texture) {
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        }
        return texture;
    };
    m_frameTarget = create();
    m_presentedTarget = create();
    m_outgoingTarget = create();
    if (!m_frameTarget || !m_presentedTarget || !m_outgoingTarget) {
        DestroyFrameTargets();
        return false;
    }
    SDL_Texture* previous = SDL_GetRenderTarget(m_renderer);
    SDL_SetRenderTarget(m_renderer, m_presentedTarget);
    SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 0);
    SDL_RenderClear(m_renderer);
    SDL_SetRenderTarget(m_renderer, m_outgoingTarget);
    SDL_RenderClear(m_renderer);
    SDL_SetRenderTarget(m_renderer, previous);
    return true;
}

void Renderer2D::DestroyFrameTargets() {
    m_compositor.InvalidateCustomTransitionTargets();
    if (m_frameTarget) SDL_DestroyTexture(m_frameTarget);
    if (m_presentedTarget) SDL_DestroyTexture(m_presentedTarget);
    if (m_outgoingTarget) SDL_DestroyTexture(m_outgoingTarget);
    if (m_presentationHoldTarget) SDL_DestroyTexture(m_presentationHoldTarget);
    m_frameTarget = m_presentedTarget = m_outgoingTarget = nullptr;
    m_presentationHoldTarget = nullptr;
    DestroyRuleScreenTargets();
    m_presentationHoldActive = false;
    m_framePreviousTarget = nullptr;
    m_frameRecording = false;
    FinishScreenEffect(ScreenEffectStatus::Cancelled);
}

void Renderer2D::ResetRenderTargets() {
    if (m_frameRecording) return;
    DestroyMaskTargets();
    DestroyFrameTargets();
    PX_LOG_INFO("Renderer2D render targets reset; recreating on next frame");
}

bool Renderer2D::BeginFrame(const Color clearColor) {
    if (!EnsureFrameTargets() || m_frameRecording) return false;
    m_framePreviousTarget = SDL_GetRenderTarget(m_renderer);
    if (!SDL_SetRenderTarget(m_renderer, m_frameTarget)) return false;
    SDL_SetRenderDrawColor(m_renderer, clearColor.r, clearColor.g, clearColor.b,
                           clearColor.a);
    SDL_RenderClear(m_renderer);
    m_frameRecording = true;
    return true;
}

void Renderer2D::EndFrame() {
    if (!m_frameRecording) return;
    m_frameRecording = false;
    // Keep the composited result, not merely the incoming scene, as the
    // outgoing snapshot for a transition started while another is playing.
    SDL_SetRenderTarget(m_renderer, m_presentedTarget);
    PresentScreenFrame();
    SDL_SetRenderTarget(m_renderer, m_framePreviousTarget);
    SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
    SDL_RenderClear(m_renderer);
    const SDL_FRect viewport{0.0f, 0.0f, static_cast<float>(m_logicalW),
                             static_cast<float>(m_logicalH)};
    SDL_RenderTexture(m_renderer, m_presentedTarget, nullptr, &viewport);
    m_framePreviousTarget = nullptr;
}

bool Renderer2D::BeginPresentationHold() {
    if (!EnsureFrameTargets()) return false;
    if (!m_presentationHoldTarget) {
        m_presentationHoldTarget = SDL_CreateTexture(
            m_renderer, kTargetFormat, SDL_TEXTUREACCESS_TARGET,
            m_logicalW, m_logicalH);
        if (!m_presentationHoldTarget) return false;
        (void)SDL_SetTextureBlendMode(m_presentationHoldTarget,
                                      SDL_BLENDMODE_BLEND);
        (void)SDL_SetTextureScaleMode(m_presentationHoldTarget,
                                      SDL_SCALEMODE_LINEAR);
    }
    SDL_Texture* previous = SDL_GetRenderTarget(m_renderer);
    if (!SDL_SetRenderTarget(m_renderer, m_presentationHoldTarget)) return false;
    std::uint8_t red = 0, green = 0, blue = 0, alpha = 0;
    (void)SDL_GetRenderDrawColor(m_renderer, &red, &green, &blue, &alpha);
    (void)SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 0);
    (void)SDL_RenderClear(m_renderer);
    const SDL_FRect viewport{0.0f, 0.0f, static_cast<float>(m_logicalW),
                             static_cast<float>(m_logicalH)};
    const bool copied = SDL_RenderTexture(m_renderer, m_presentedTarget,
                                          nullptr, &viewport);
    (void)SDL_SetRenderTarget(m_renderer, previous);
    (void)SDL_SetRenderDrawColor(m_renderer, red, green, blue, alpha);
    if (copied) m_presentationHoldActive = true;
    return copied;
}

ScreenEffectHandle Renderer2D::PlayScreenEffect(const std::string_view id,
    const float durationSeconds,
    const CustomEffectNamedParameters* parameters,
    const bool easeInOut) {
    const auto found = m_screenEffectDefinitions.find(std::string(id));
    if (found == m_screenEffectDefinitions.end() ||
        !std::isfinite(durationSeconds) || durationSeconds < 0.0f)
        return 0;
    std::array<std::array<float, 4>, 8> resolvedParameters{};
    if (found->second.operation == "custom") {
        const auto resolved = parameters
            ? m_compositor.ResolveCustomEffectParameters(
                  found->second.customEffect, *parameters)
            : m_compositor.CustomEffectDefaults(found->second.customEffect);
        if (!resolved) return 0;
        resolvedParameters = *resolved;
    } else if (parameters) {
        return 0;
    }
    if (m_activeScreenEffect)
        FinishScreenEffect(ScreenEffectStatus::Cancelled);
    const ScreenEffectHandle handle = m_nextScreenEffectHandle++;
    if (found->second.operation == "none" || durationSeconds <= 0.0f) {
        m_screenEffectStates[handle] = ScreenEffectStatus::Completed;
        return handle;
    }
    if (!EnsureFrameTargets()) return 0;
    SDL_Texture* previous = SDL_GetRenderTarget(m_renderer);
    SDL_SetRenderTarget(m_renderer, m_outgoingTarget);
    SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 0);
    SDL_RenderClear(m_renderer);
    const SDL_FRect viewport{0.0f, 0.0f, static_cast<float>(m_logicalW),
                             static_cast<float>(m_logicalH)};
    SDL_RenderTexture(m_renderer, m_presentedTarget, nullptr, &viewport);
    SDL_SetRenderTarget(m_renderer, previous);
    ScreenEffectPlayback playback;
    playback.handle = handle;
    playback.effect = found->second.id;
    playback.durationSeconds = durationSeconds;
    playback.easeInOut = easeInOut;
    playback.status = ScreenEffectStatus::Playing;
    playback.randomSeed = static_cast<float>(
        (handle * 2654435761ull) & 0x00ffffffu);
    playback.parameters = resolvedParameters;
    m_activeScreenEffect = std::move(playback);
    m_screenEffectStates[handle] = ScreenEffectStatus::Playing;
    return handle;
}

ScreenEffectHandle Renderer2D::PlayRuleScreenTransition(
    const std::string& rulePath, const float durationSeconds, const int vague) {
    if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0f ||
        m_logicalW <= 0 || m_logicalH <= 0)
        return PlayScreenEffect("crossfade", durationSeconds);
    const int width = m_logicalW;
    const int height = m_logicalH;
    if (!m_ruleScreen || m_ruleScreen->path != rulePath ||
        m_ruleScreen->width != width || m_ruleScreen->height != height) {
        SDL_Surface* rule = m_assets.LoadSurface(rulePath);
        if (!rule) return PlayScreenEffect("crossfade", durationSeconds);
        SDL_Surface* scaled = rule->w == width && rule->h == height
            ? rule : SDL_ScaleSurface(rule, width, height, SDL_SCALEMODE_LINEAR);
        if (scaled != rule) SDL_DestroySurface(rule);
        if (!scaled) return PlayScreenEffect("crossfade", durationSeconds);
        RuleScreenTransition built;
        built.path = rulePath;
        built.width = width;
        built.height = height;
        built.values.resize(static_cast<std::size_t>(width) * height);
        built.rowMin.assign(static_cast<std::size_t>(height), 255);
        built.rowMax.assign(static_cast<std::size_t>(height), 0);
        built.maskPixels.assign(static_cast<std::size_t>(width) * height * 4u, 255);
        const auto* pixels = static_cast<const std::uint8_t*>(scaled->pixels);
        for (int row = 0; row < height; ++row) {
            const std::uint8_t* line = pixels + static_cast<std::size_t>(row) * scaled->pitch;
            std::uint8_t* out = built.values.data() + static_cast<std::size_t>(row) * width;
            std::uint8_t low = 255, high = 0;
            for (int col = 0; col < width; ++col) {
                const std::uint8_t value = line[col * 4];  // R of RGBA32
                out[col] = value;
                low = std::min(low, value);
                high = std::max(high, value);
            }
            built.rowMin[static_cast<std::size_t>(row)] = low;
            built.rowMax[static_cast<std::size_t>(row)] = high;
        }
        SDL_DestroySurface(scaled);
        m_ruleScreen = std::move(built);
    }
    m_ruleScreen->vague = std::clamp(vague, 1, 255);
    m_ruleScreen->uploaded = false;
    if (!m_ruleScreenMask) {
        m_ruleScreenMask = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ABGR8888,
                                             SDL_TEXTUREACCESS_STREAMING, width, height);
        m_ruleScreenMasked = SDL_CreateTexture(m_renderer, kTargetFormat,
                                               SDL_TEXTUREACCESS_TARGET, width, height);
        // Keeps the outgoing colour and multiplies its alpha by the mask.
        const SDL_BlendMode maskAlpha = SDL_ComposeCustomBlendMode(
            SDL_BLENDFACTOR_ZERO, SDL_BLENDFACTOR_ONE, SDL_BLENDOPERATION_ADD,
            SDL_BLENDFACTOR_DST_ALPHA, SDL_BLENDFACTOR_ZERO, SDL_BLENDOPERATION_ADD);
        if (!m_ruleScreenMask || !m_ruleScreenMasked ||
            !SDL_SetTextureBlendMode(m_ruleScreenMask, maskAlpha)) {
            DestroyRuleScreenTargets();
            return PlayScreenEffect("crossfade", durationSeconds);
        }
        (void)SDL_SetTextureBlendMode(m_ruleScreenMasked, SDL_BLENDMODE_BLEND);
    }
    return PlayScreenEffect("rule", durationSeconds);
}

void Renderer2D::RenderRuleScreenTransition(const float progress) {
    const SDL_FRect viewport{0.0f, 0.0f, static_cast<float>(m_logicalW),
                             static_cast<float>(m_logicalH)};
    if (!m_ruleScreen || !m_ruleScreenMask || !m_ruleScreenMasked) {
        SDL_RenderTexture(m_renderer, m_frameTarget, nullptr, &viewport);
        return;
    }
    auto& rule = *m_ruleScreen;
    // Rule values below the sweeping threshold have dissolved; `vague` rule
    // levels form the soft edge. Only rows holding a changed level are
    // rewritten, so a sweeping band costs a band, not the whole mask.
    const float vagueLevels = static_cast<float>(rule.vague);
    const float threshold = progress * (255.0f + vagueLevels);
    std::array<std::uint8_t, 256> alpha{};
    int changedLow = 256;
    int changedHigh = -1;
    for (int value = 0; value < 256; ++value) {
        const float a = (static_cast<float>(value) + vagueLevels - threshold) *
                        (255.0f / vagueLevels);
        alpha[static_cast<std::size_t>(value)] =
            static_cast<std::uint8_t>(std::clamp(a, 0.0f, 255.0f));
        if (!rule.uploaded || alpha[static_cast<std::size_t>(value)] !=
                                  rule.alphaByRule[static_cast<std::size_t>(value)]) {
            changedLow = std::min(changedLow, value);
            changedHigh = std::max(changedHigh, value);
        }
    }
    if (changedHigh >= 0) {
        const auto width = static_cast<std::size_t>(rule.width);
        int firstRow = -1;
        int lastRow = -1;
        for (int row = 0; row < rule.height; ++row) {
            const auto index = static_cast<std::size_t>(row);
            if (rule.uploaded && (rule.rowMax[index] < changedLow ||
                                  rule.rowMin[index] > changedHigh))
                continue;
            const std::uint8_t* values = rule.values.data() + index * width;
            std::uint8_t* pixels = rule.maskPixels.data() + index * width * 4u;
            for (std::size_t col = 0; col < width; ++col)
                pixels[col * 4u + 3u] = alpha[values[col]];
            if (firstRow < 0) firstRow = row;
            lastRow = row;
        }
        if (firstRow >= 0) {
            const SDL_Rect rows{0, firstRow, rule.width, lastRow - firstRow + 1};
            (void)SDL_UpdateTexture(
                m_ruleScreenMask, &rows,
                rule.maskPixels.data() + static_cast<std::size_t>(firstRow) * width * 4u,
                static_cast<int>(width * 4u));
        }
        rule.alphaByRule = alpha;
        rule.uploaded = true;
    }
    SDL_Texture* previous = SDL_GetRenderTarget(m_renderer);
    SDL_SetRenderTarget(m_renderer, m_ruleScreenMasked);
    (void)SDL_SetTextureBlendMode(m_outgoingTarget, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(m_renderer, m_outgoingTarget, nullptr, &viewport);
    (void)SDL_SetTextureBlendMode(m_outgoingTarget, SDL_BLENDMODE_BLEND);
    SDL_RenderTexture(m_renderer, m_ruleScreenMask, nullptr, &viewport);
    SDL_SetRenderTarget(m_renderer, previous);
    SDL_RenderTexture(m_renderer, m_frameTarget, nullptr, &viewport);
    SDL_RenderTexture(m_renderer, m_ruleScreenMasked, nullptr, &viewport);
}

void Renderer2D::DestroyRuleScreenTargets() {
    if (m_ruleScreenMask) SDL_DestroyTexture(m_ruleScreenMask);
    if (m_ruleScreenMasked) SDL_DestroyTexture(m_ruleScreenMasked);
    m_ruleScreenMask = m_ruleScreenMasked = nullptr;
    m_ruleScreen.reset();
}

bool Renderer2D::StopScreenEffect(const ScreenEffectHandle handle) {
    if (!m_activeScreenEffect || m_activeScreenEffect->handle != handle) return false;
    FinishScreenEffect(ScreenEffectStatus::Stopped);
    return true;
}

bool Renderer2D::CancelScreenEffect(const ScreenEffectHandle handle) {
    if (!m_activeScreenEffect || m_activeScreenEffect->handle != handle) return false;
    FinishScreenEffect(ScreenEffectStatus::Cancelled);
    return true;
}

bool Renderer2D::PauseScreenEffect(const ScreenEffectHandle handle,
                                   const bool paused) {
    if (!m_activeScreenEffect || m_activeScreenEffect->handle != handle)
        return false;
    m_activeScreenEffect->paused = paused;
    return true;
}

bool Renderer2D::ScreenEffectPlaying(const ScreenEffectHandle handle) const {
    return m_activeScreenEffect && m_activeScreenEffect->handle == handle &&
           m_activeScreenEffect->status == ScreenEffectStatus::Playing;
}

ScreenEffectStatus Renderer2D::ScreenEffectState(
    const ScreenEffectHandle handle) const {
    const auto found = m_screenEffectStates.find(handle);
    return found == m_screenEffectStates.end() ? ScreenEffectStatus::Unknown
                                               : found->second;
}

std::optional<ScreenEffectPlayback> Renderer2D::ActiveScreenEffect() const {
    return m_activeScreenEffect;
}

void Renderer2D::UpdateScreenEffects(const float deltaSeconds) {
    if (!m_activeScreenEffect || m_activeScreenEffect->paused ||
        m_presentationHoldActive ||
        !std::isfinite(deltaSeconds) ||
        deltaSeconds < 0.0f)
        return;
    m_activeScreenEffect->elapsedSeconds = std::min(
        m_activeScreenEffect->durationSeconds,
        m_activeScreenEffect->elapsedSeconds + deltaSeconds);
    if (m_activeScreenEffect->elapsedSeconds >=
        m_activeScreenEffect->durationSeconds)
        FinishScreenEffect(ScreenEffectStatus::Completed);
}

void Renderer2D::FinishScreenEffect(const ScreenEffectStatus status) {
    if (!m_activeScreenEffect) return;
    m_screenEffectStates[m_activeScreenEffect->handle] = status;
    m_activeScreenEffect.reset();
    constexpr std::size_t kTerminalHistoryLimit = 256;
    if (m_screenEffectStates.size() > kTerminalHistoryLimit) {
        const ScreenEffectHandle floor =
            m_nextScreenEffectHandle > kTerminalHistoryLimit
                ? m_nextScreenEffectHandle - kTerminalHistoryLimit
                : 0;
        std::erase_if(m_screenEffectStates,
                      [floor](const auto& entry) { return entry.first < floor; });
    }
}

void Renderer2D::PresentScreenFrame() {
    SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 255);
    SDL_RenderClear(m_renderer);
    if (m_presentationHoldActive) {
        const SDL_FRect viewport{0.0f, 0.0f, static_cast<float>(m_logicalW),
                                 static_cast<float>(m_logicalH)};
        SDL_RenderTexture(m_renderer, m_presentationHoldTarget,
                          nullptr, &viewport);
        return;
    }
    if (!m_activeScreenEffect) {
        const SDL_FRect viewport{0.0f, 0.0f, static_cast<float>(m_logicalW),
                                 static_cast<float>(m_logicalH)};
        SDL_RenderTexture(m_renderer, m_frameTarget, nullptr, &viewport);
        return;
    }
    const auto definition =
        m_screenEffectDefinitions.find(m_activeScreenEffect->effect);
    if (definition == m_screenEffectDefinitions.end()) {
        FinishScreenEffect(ScreenEffectStatus::Cancelled);
        const SDL_FRect viewport{0.0f, 0.0f, static_cast<float>(m_logicalW),
                                 static_cast<float>(m_logicalH)};
        SDL_RenderTexture(m_renderer, m_frameTarget, nullptr, &viewport);
        return;
    }
    float progress = std::clamp(
        m_activeScreenEffect->elapsedSeconds /
            std::max(0.0001f, m_activeScreenEffect->durationSeconds),
        0.0f, 1.0f);
    const std::string& operation = definition->second.operation;
    if (m_activeScreenEffect->easeInOut &&
        (operation == "fade" || operation == "crossfade" ||
         operation == "slide-left" || operation == "slide-right"))
        progress = progress * progress * (3.0f - 2.0f * progress);
    RenderScreenEffect(definition->second, progress);
}

void Renderer2D::RenderScreenEffect(const ScreenEffectDefinition& definition,
                                    const float progress) {
    const float width = static_cast<float>(m_logicalW);
    const float height = static_cast<float>(m_logicalH);
    const SDL_FRect viewport{0.0f, 0.0f, width, height};
    const auto drawAlpha = [&](SDL_Texture* texture, const float alpha,
                               const SDL_FRect& destination) {
        SDL_SetTextureAlphaMod(texture, static_cast<std::uint8_t>(
                                            std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
        SDL_RenderTexture(m_renderer, texture, nullptr, &destination);
        SDL_SetTextureAlphaMod(texture, 255);
    };
    if (definition.operation == "custom") {
        if (!m_compositor.ApplyCustomTransition(
                definition.customEffect, m_outgoingTarget, m_frameTarget,
                progress,
                m_activeScreenEffect ? m_activeScreenEffect->randomSeed : 0.0f,
                m_activeScreenEffect ? m_activeScreenEffect->parameters
                                     : std::array<std::array<float, 4>, 8>{})) {
            drawAlpha(m_outgoingTarget, 1.0f, viewport);
            drawAlpha(m_frameTarget, progress, viewport);
        }
        return;
    }
    if (definition.operation == "fade") {
        if (progress < 0.5f)
            drawAlpha(m_outgoingTarget, 1.0f - progress * 2.0f, viewport);
        else
            drawAlpha(m_frameTarget, progress * 2.0f - 1.0f, viewport);
        return;
    }
    if (definition.operation == "crossfade") {
        drawAlpha(m_outgoingTarget, 1.0f, viewport);
        drawAlpha(m_frameTarget, progress, viewport);
        return;
    }
    if (definition.operation == "rule") {
        RenderRuleScreenTransition(progress);
        return;
    }
    if (definition.operation == "slide-left" ||
        definition.operation == "slide-right") {
        const float direction = definition.operation == "slide-left" ? -1.0f : 1.0f;
        SDL_FRect outgoing{direction * progress * width, 0.0f, width, height};
        SDL_FRect incoming{direction * (progress - 1.0f) * width, 0.0f, width,
                           height};
        SDL_RenderTexture(m_renderer, m_outgoingTarget, nullptr, &outgoing);
        SDL_RenderTexture(m_renderer, m_frameTarget, nullptr, &incoming);
        return;
    }
    if (definition.operation == "tiles") {
        const int columns = definition.columns;
        const int rows = definition.rows;
        const int tileCount = columns * rows;
        for (int row = 0; row < rows; ++row) {
            for (int column = 0; column < columns; ++column) {
                int order = row * columns + column;
                if (definition.order == "column-major") order = column * rows + row;
                else if (definition.order == "reverse") order = tileCount - 1 - order;
                const float delay = tileCount <= 1
                                        ? 0.0f
                                        : static_cast<float>(order) /
                                              static_cast<float>(tileCount - 1);
                const float local = std::clamp(
                    (progress * (1.0f + definition.stagger) -
                     delay * definition.stagger),
                    0.0f, 1.0f);
                const float x0 = width * static_cast<float>(column) / columns;
                const float x1 = width * static_cast<float>(column + 1) / columns;
                const float y0 = height * static_cast<float>(row) / rows;
                const float y1 = height * static_cast<float>(row + 1) / rows;
                const float scale = std::abs(std::cos(local * 3.14159265358979323846f));
                const SDL_FRect source{x0, y0, x1 - x0, y1 - y0};
                const SDL_FRect destination{x0 + (x1 - x0) * (1.0f - scale) * 0.5f,
                                            y0, (x1 - x0) * scale, y1 - y0};
                SDL_RenderTexture(m_renderer,
                                  local < 0.5f ? m_outgoingTarget : m_frameTarget,
                                  &source, &destination);
            }
        }
        return;
    }
    SDL_RenderTexture(m_renderer, m_frameTarget, nullptr, &viewport);
}

Vec2 Renderer2D::TransformPoint(const Vec2 point) const {const auto& t=m_transformStack.back();return {t.a*point.x+t.c*point.y+t.tx+m_camX,t.b*point.x+t.d*point.y+t.ty+m_camY};}
Color Renderer2D::TransformColor(const Color color) const {const auto tint=m_transformStack.back().modulate;return {static_cast<std::uint8_t>((static_cast<unsigned>(color.r)*tint.r)/255),static_cast<std::uint8_t>((static_cast<unsigned>(color.g)*tint.g)/255),static_cast<std::uint8_t>((static_cast<unsigned>(color.b)*tint.b)/255),static_cast<std::uint8_t>((static_cast<unsigned>(color.a)*tint.a)/255)};}
Rect Renderer2D::TransformBounds(const Rect rect) const {const std::array<Vec2,4> points{TransformPoint({rect.x,rect.y}),TransformPoint({rect.x+rect.w,rect.y}),TransformPoint({rect.x+rect.w,rect.y+rect.h}),TransformPoint({rect.x,rect.y+rect.h})};float left=points[0].x,right=left,top=points[0].y,bottom=top;for(const auto point:points){left=std::min(left,point.x);right=std::max(right,point.x);top=std::min(top,point.y);bottom=std::max(bottom,point.y);}return {left,top,right-left,bottom-top};}
void Renderer2D::PushTransform(const Vec2 pivot,const Vec2 scale,const float rotationDegrees,const Color modulate){const float radians=rotationDegrees*3.14159265358979323846f/180.0f,cs=std::cos(radians),sn=std::sin(radians);TransformState local{cs*scale.x,sn*scale.x,-sn*scale.y,cs*scale.y,0,0,modulate};local.tx=pivot.x-(local.a*pivot.x+local.c*pivot.y);local.ty=pivot.y-(local.b*pivot.x+local.d*pivot.y);const auto& parent=m_transformStack.back();TransformState combined;combined.a=parent.a*local.a+parent.c*local.b;combined.b=parent.b*local.a+parent.d*local.b;combined.c=parent.a*local.c+parent.c*local.d;combined.d=parent.b*local.c+parent.d*local.d;combined.tx=parent.a*local.tx+parent.c*local.ty+parent.tx;combined.ty=parent.b*local.tx+parent.d*local.ty+parent.ty;combined.modulate={static_cast<std::uint8_t>((static_cast<unsigned>(parent.modulate.r)*modulate.r)/255),static_cast<std::uint8_t>((static_cast<unsigned>(parent.modulate.g)*modulate.g)/255),static_cast<std::uint8_t>((static_cast<unsigned>(parent.modulate.b)*modulate.b)/255),static_cast<std::uint8_t>((static_cast<unsigned>(parent.modulate.a)*modulate.a)/255)};m_transformStack.push_back(combined);}

bool Renderer2D::BeginRuleMask(const std::string& rulePath,
                               const float threshold, const float vague) {
    if (!m_compositor.RuleMaskSupported() || !m_assets.HasResidentTexture(rulePath))
        return false;
    const TextureHandle rule = m_assets.Texture(rulePath);
    auto* native = static_cast<SDL_Texture*>(rule.Native(TextureBackend::SdlRenderer));
    return native && m_compositor.BeginRuleMask(native, threshold, vague);
}

void Renderer2D::PushAffineTransform(const AffineTransform2D& transform,
                                     const Color modulate) {
    const auto finite = [](const float value) { return std::isfinite(value); };
    if (!finite(transform.a) || !finite(transform.b) ||
        !finite(transform.c) || !finite(transform.d) ||
        !finite(transform.tx) || !finite(transform.ty)) {
        // Preserve stack balance for retained draw command streams. An invalid
        // transform is a no-op rather than a source of NaN renderer geometry.
        m_transformStack.push_back(m_transformStack.back());
        return;
    }
    const auto& parent = m_transformStack.back();
    TransformState combined;
    combined.a = parent.a * transform.a + parent.c * transform.b;
    combined.b = parent.b * transform.a + parent.d * transform.b;
    combined.c = parent.a * transform.c + parent.c * transform.d;
    combined.d = parent.b * transform.c + parent.d * transform.d;
    combined.tx = parent.a * transform.tx + parent.c * transform.ty +
                  parent.tx;
    combined.ty = parent.b * transform.tx + parent.d * transform.ty +
                  parent.ty;
    combined.modulate = {
        static_cast<std::uint8_t>(
            (static_cast<unsigned>(parent.modulate.r) * modulate.r) / 255),
        static_cast<std::uint8_t>(
            (static_cast<unsigned>(parent.modulate.g) * modulate.g) / 255),
        static_cast<std::uint8_t>(
            (static_cast<unsigned>(parent.modulate.b) * modulate.b) / 255),
        static_cast<std::uint8_t>(
            (static_cast<unsigned>(parent.modulate.a) * modulate.a) / 255)};
    m_transformStack.push_back(combined);
}
void Renderer2D::PopTransform(){if(m_transformStack.size()>1)m_transformStack.pop_back();}

void Renderer2D::SetPreviewContext(float displayPixelsPerLogical, bool clarityCompensation) {
    m_displayPixelsPerLogical = std::max(0.01f, displayPixelsPerLogical);
    m_clarityCompensation = clarityCompensation;
}

void Renderer2D::DrawBorder(const Rect& rect, float width, float, Color color) {
    if (rect.w <= 0.0f || rect.h <= 0.0f || width <= 0.0f) return;
    const float pixel = 1.0f / m_displayPixelsPerLogical;
    const float stroke = m_clarityCompensation ? std::max(width, pixel) : width;
    const auto align = [&](float value) {
        return m_clarityCompensation ? std::round(value / pixel) * pixel : value;
    };
    const float left=align(rect.x),top=align(rect.y),right=align(rect.x+rect.w),bottom=align(rect.y+rect.h);
    DrawRect({left,top,std::max(0.0f,right-left),stroke},color);
    DrawRect({left,std::max(top,bottom-stroke),std::max(0.0f,right-left),stroke},color);
    DrawRect({left,top+stroke,stroke,std::max(0.0f,bottom-top-stroke*2)},color);
    DrawRect({std::max(left,right-stroke),top+stroke,stroke,std::max(0.0f,bottom-top-stroke*2)},color);
}

void Renderer2D::PushClip(const Rect& requested) {
    const bool valid = std::isfinite(requested.x) &&
                       std::isfinite(requested.y) &&
                       std::isfinite(requested.w) &&
                       std::isfinite(requested.h) && requested.w > 0.0f &&
                       requested.h > 0.0f;
    Rect clip = valid ? TransformBounds(requested) : Rect{};
    if (!m_clipStack.empty()) {
        const Rect parent = m_clipStack.back();
        const float left = std::max(parent.x, clip.x);
        const float top = std::max(parent.y, clip.y);
        const float right = std::min(parent.x + parent.w, clip.x + clip.w);
        const float bottom = std::min(parent.y + parent.h, clip.y + clip.h);
        clip = {left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
    }
    m_clipStack.push_back(clip);
    const int left=static_cast<int>(std::floor(clip.x)),top=static_cast<int>(std::floor(clip.y));
    const int right=static_cast<int>(std::ceil(clip.x+clip.w)),bottom=static_cast<int>(std::ceil(clip.y+clip.h));
    const SDL_Rect value{left,top,std::max(0,right-left),std::max(0,bottom-top)};
    SDL_SetRenderClipRect(m_renderer, &value);
}

void Renderer2D::PopClip() {
    if (m_clipStack.empty()) return;
    m_clipStack.pop_back();
    if (m_clipStack.empty()) {
        SDL_SetRenderClipRect(m_renderer, nullptr);
    } else {
        const Rect clip = m_clipStack.back();
        const int left=static_cast<int>(std::floor(clip.x)),top=static_cast<int>(std::floor(clip.y));
        const int right=static_cast<int>(std::ceil(clip.x+clip.w)),bottom=static_cast<int>(std::ceil(clip.y+clip.h));
        const SDL_Rect value{left,top,std::max(0,right-left),std::max(0,bottom-top)};
        SDL_SetRenderClipRect(m_renderer, &value);
    }
}

void Renderer2D::Blit(SDL_Texture* texture, const Rect& dst, std::uint8_t alpha) {
    if (!texture) {
        return;
    }
    const auto p0=TransformPoint({dst.x,dst.y}),p1=TransformPoint({dst.x+dst.w,dst.y}),p2=TransformPoint({dst.x+dst.w,dst.y+dst.h}),p3=TransformPoint({dst.x,dst.y+dst.h});const Color color=TransformColor({255,255,255,alpha});const SDL_FColor fc=ToFColor(color);SDL_Vertex vertices[4]{{{p0.x,p0.y},fc,{0,0}},{{p1.x,p1.y},fc,{1,0}},{{p2.x,p2.y},fc,{1,1}},{{p3.x,p3.y},fc,{0,1}}};constexpr int indices[6]{0,1,2,0,2,3};RenderGeometry(texture,vertices,4,indices,6);
}

void Renderer2D::BlitRegion(SDL_Texture* texture,const Rect& sourcePixels,const Rect& dst,const std::uint8_t alpha){
    const auto finiteRect=[](const Rect& value){return std::isfinite(value.x)&&std::isfinite(value.y)&&std::isfinite(value.w)&&std::isfinite(value.h);};
    if(!texture||!finiteRect(dst)||!finiteRect(sourcePixels)||dst.w<=0||dst.h<=0||sourcePixels.w<=0||sourcePixels.h<=0)return;float nativeWidth=0,nativeHeight=0;if(!SDL_GetTextureSize(texture,&nativeWidth,&nativeHeight)||nativeWidth<=0||nativeHeight<=0)return;
    const float sourceLeft=std::clamp(sourcePixels.x,0.0f,nativeWidth),sourceTop=std::clamp(sourcePixels.y,0.0f,nativeHeight),sourceRight=std::clamp(sourcePixels.x+sourcePixels.w,0.0f,nativeWidth),sourceBottom=std::clamp(sourcePixels.y+sourcePixels.h,0.0f,nativeHeight);if(sourceRight<=sourceLeft||sourceBottom<=sourceTop)return;
    const float xFraction=(sourceLeft-sourcePixels.x)/sourcePixels.w,yFraction=(sourceTop-sourcePixels.y)/sourcePixels.h,rightFraction=(sourceRight-sourcePixels.x)/sourcePixels.w,bottomFraction=(sourceBottom-sourcePixels.y)/sourcePixels.h;
    const Rect clippedDestination{dst.x+dst.w*xFraction,dst.y+dst.h*yFraction,dst.w*(rightFraction-xFraction),dst.h*(bottomFraction-yFraction)};
    const float u0=sourceLeft/nativeWidth,v0=sourceTop/nativeHeight,u1=sourceRight/nativeWidth,v1=sourceBottom/nativeHeight;
    const auto p0=TransformPoint({clippedDestination.x,clippedDestination.y}),p1=TransformPoint({clippedDestination.x+clippedDestination.w,clippedDestination.y}),p2=TransformPoint({clippedDestination.x+clippedDestination.w,clippedDestination.y+clippedDestination.h}),p3=TransformPoint({clippedDestination.x,clippedDestination.y+clippedDestination.h});const SDL_FColor color=ToFColor(TransformColor({255,255,255,alpha}));
    SDL_Vertex vertices[4]{{{p0.x,p0.y},color,{u0,v0}},{{p1.x,p1.y},color,{u1,v0}},{{p2.x,p2.y},color,{u1,v1}},{{p3.x,p3.y},color,{u0,v1}}};constexpr int indices[6]{0,1,2,0,2,3};RenderGeometry(texture,vertices,4,indices,6);
}

void Renderer2D::DrawImage(const std::string& path, const Rect& dst,
                           const std::uint8_t alpha) {
    Blit(NativeTexture(m_assets.Texture(path)), dst, alpha);
}

void Renderer2D::DrawImageBlended(const std::string& path, const Rect& dst,
                                  const std::uint8_t alpha,
                                  const ImageBlendMode blendMode) {
    if (blendMode == ImageBlendMode::Normal) return DrawImage(path, dst, alpha);
    SDL_Texture* texture = NativeTexture(m_assets.Texture(path));
    if (!texture) return;
    SDL_BlendMode previous = SDL_BLENDMODE_BLEND;
    if (!SDL_GetTextureBlendMode(texture, &previous) ||
        !SDL_SetTextureBlendMode(texture, NativeBlendMode(blendMode))) {
        Blit(texture, dst, alpha);
        return;
    }
    Blit(texture, dst, alpha);
    (void)SDL_SetTextureBlendMode(texture, previous);
}

void Renderer2D::DrawImageInRect(const std::string& path, const Rect& bounds,
                                 ContentScaleMode mode, HorizontalAlignment horizontal,
                                 VerticalAlignment vertical, std::uint8_t alpha) {
    const TextureHandle handle=m_assets.Texture(path);SDL_Texture* texture=NativeTexture(handle);if(!texture||bounds.w<=0||bounds.h<=0)return;
    int tw=0,th=0;AssetCache::TextureSize(handle,tw,th);if(tw<=0||th<=0)return;
    if(mode==ContentScaleMode::Stretch){Blit(texture,bounds,alpha);return;}
    float scale=1.0f;
    if(mode==ContentScaleMode::Fit)scale=std::min(bounds.w/static_cast<float>(tw),bounds.h/static_cast<float>(th));
    else if(mode==ContentScaleMode::Fill)scale=std::max(bounds.w/static_cast<float>(tw),bounds.h/static_cast<float>(th));
    const float w=tw*scale,h=th*scale;float x=bounds.x,y=bounds.y;
    if(horizontal==HorizontalAlignment::Center)x+=(bounds.w-w)*.5f;else if(horizontal==HorizontalAlignment::Right)x+=bounds.w-w;
    if(vertical==VerticalAlignment::Center)y+=(bounds.h-h)*.5f;else if(vertical==VerticalAlignment::Bottom)y+=bounds.h-h;
    PushClip(bounds);Blit(texture,{x,y,w,h},alpha);PopClip();
}

void Renderer2D::DrawNinePatch(const std::string& path,const Rect& bounds,Rect margins,const bool drawCenter,const std::uint8_t alpha,const float marginScale){
    const TextureHandle handle=m_assets.Texture(path);SDL_Texture* texture=NativeTexture(handle);if(!texture||bounds.w<=0||bounds.h<=0)return;int width=0,height=0;AssetCache::TextureSize(handle,width,height);if(width<=0||height<=0)return;
    float left=std::clamp(margins.x,0.0f,static_cast<float>(width)),top=std::clamp(margins.y,0.0f,static_cast<float>(height));
    float right=std::clamp(margins.w,0.0f,static_cast<float>(width)-left),bottom=std::clamp(margins.h,0.0f,static_cast<float>(height)-top);
    const float scale=std::isfinite(marginScale)&&marginScale>0.0f?marginScale:1.0f;
    const float destinationLeft=std::min(left*scale,bounds.w*.5f),destinationRight=std::min(right*scale,bounds.w-destinationLeft);
    const float destinationTop=std::min(top*scale,bounds.h*.5f),destinationBottom=std::min(bottom*scale,bounds.h-destinationTop);
    const float sourceX[4]{0,left,static_cast<float>(width)-right,static_cast<float>(width)};
    const float sourceY[4]{0,top,static_cast<float>(height)-bottom,static_cast<float>(height)};
    const float destinationX[4]{bounds.x,bounds.x+destinationLeft,bounds.x+bounds.w-destinationRight,bounds.x+bounds.w};
    const float destinationY[4]{bounds.y,bounds.y+destinationTop,bounds.y+bounds.h-destinationBottom,bounds.y+bounds.h};
    // The stretched middle row and column sample half a texel inside their
    // own pixels, so filtering does not bleed the border into them.
    const auto inset=[](float from,float to){return to-from>1.0f?std::pair{from+.5f,to-.5f}:std::pair{from,to};};
    for(int y=0;y<3;++y)for(int x=0;x<3;++x){
        if(!drawCenter&&x==1&&y==1)continue;
        auto [x0,x1]=x==1?inset(sourceX[1],sourceX[2]):std::pair{sourceX[x],sourceX[x+1]};
        auto [y0,y1]=y==1?inset(sourceY[1],sourceY[2]):std::pair{sourceY[y],sourceY[y+1]};
        if(x1-x0<=0.0f||y1-y0<=0.0f)continue;
        BlitRegion(texture,{x0,y0,x1-x0,y1-y0},{destinationX[x],destinationY[y],destinationX[x+1]-destinationX[x],destinationY[y+1]-destinationY[y]},alpha);
    }
}

void Renderer2D::DrawTexture(const TextureHandle texture, const Rect& dst,
                             const std::uint8_t alpha) {
    Blit(NativeTexture(texture), dst, alpha);
}

void Renderer2D::DrawImageRegion(const std::string& path,
                                 const Rect& sourcePixels, const Rect& dst,
                                 const std::uint8_t alpha) {
    BlitRegion(NativeTexture(m_assets.Texture(path)), sourcePixels, dst, alpha);
}

void Renderer2D::DrawImageRegionBlended(const std::string& path,
                                        const Rect& sourcePixels, const Rect& dst,
                                        const std::uint8_t alpha,
                                        const ImageBlendMode blendMode) {
    if (blendMode == ImageBlendMode::Normal)
        return DrawImageRegion(path, sourcePixels, dst, alpha);
    SDL_Texture* texture = NativeTexture(m_assets.Texture(path));
    if (!texture) return;
    SDL_BlendMode previous = SDL_BLENDMODE_BLEND;
    if (!SDL_GetTextureBlendMode(texture, &previous) ||
        !SDL_SetTextureBlendMode(texture, NativeBlendMode(blendMode))) {
        BlitRegion(texture, sourcePixels, dst, alpha);
        return;
    }
    BlitRegion(texture, sourcePixels, dst, alpha);
    (void)SDL_SetTextureBlendMode(texture, previous);
}

void Renderer2D::DrawTextureRegion(const TextureHandle texture,
                                   const Rect& sourcePixels, const Rect& dst,
                                   const std::uint8_t alpha) {
    BlitRegion(NativeTexture(texture), sourcePixels, dst, alpha);
}

void Renderer2D::BlitUntransformed(SDL_Texture* texture) {
    if (!m_renderer || !texture) return;
#if defined(PRISMATIX_WEB_GL_EFFECTS)
    const SDL_FColor white{1, 1, 1, 1};
    const float width = static_cast<float>(m_logicalW), height = static_cast<float>(m_logicalH);
    const SDL_Vertex vertices[4]{{{0, 0}, white, {0, 0}}, {{width, 0}, white, {1, 0}},
        {{width, height}, white, {1, 1}}, {{0, height}, white, {0, 1}}};
    constexpr int indices[6]{0, 1, 2, 0, 2, 3};
    (void)RenderGeometry(texture, vertices, 4, indices, 6);
#else
    SDL_FRect destination{0.0f, 0.0f, static_cast<float>(m_logicalW),
                          static_cast<float>(m_logicalH)};
    (void)SDL_RenderTexture(m_renderer, texture, nullptr, &destination);
#endif
}

bool Renderer2D::BeginComposition() {
    if (!m_renderer || m_logicalW <= 0 || m_logicalH <= 0) return false;
    SDL_Texture* texture = SDL_CreateTexture(
        m_renderer, kTargetFormat, SDL_TEXTUREACCESS_TARGET,
        m_logicalW, m_logicalH);
    if (!texture) return false;
    (void)SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    (void)SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    SDL_Texture* previous = SDL_GetRenderTarget(m_renderer);
    if (!SDL_SetRenderTarget(m_renderer, texture)) {
        SDL_DestroyTexture(texture);
        return false;
    }

    std::uint8_t red = 0, green = 0, blue = 0, alpha = 0;
    (void)SDL_GetRenderDrawColor(m_renderer, &red, &green, &blue, &alpha);
    const bool clipEnabled = SDL_RenderClipEnabled(m_renderer);
    SDL_Rect clip{};
    if (clipEnabled) (void)SDL_GetRenderClipRect(m_renderer, &clip);
    (void)SDL_SetRenderClipRect(m_renderer, nullptr);
    (void)SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 0);
    if (!SDL_RenderClear(m_renderer)) {
        (void)SDL_SetRenderTarget(m_renderer, previous);
        SDL_DestroyTexture(texture);
        (void)SDL_SetRenderDrawColor(m_renderer, red, green, blue, alpha);
        if (clipEnabled) (void)SDL_SetRenderClipRect(m_renderer, &clip);
        return false;
    }
    (void)SDL_SetRenderDrawColor(m_renderer, red, green, blue, alpha);
    if (clipEnabled) (void)SDL_SetRenderClipRect(m_renderer, &clip);
    m_compositionStack.push_back({texture, previous});
    return true;
}

void Renderer2D::EndComposition(
    const std::string_view effect, const float progress,
    const float randomSeed,
    const std::array<std::array<float, 4>, 8>& parameters,
    const Rect& viewport) {
    if (m_compositionStack.empty() || !m_renderer) return;
    const CompositionState composition = m_compositionStack.back();
    m_compositionStack.pop_back();
    (void)SDL_SetRenderTarget(m_renderer, composition.previousTarget);
    const bool active = BeginNodeEffect(effect, progress, randomSeed,
                                        parameters, viewport);
    BlitUntransformed(composition.texture);
    if (active) EndNodeEffect();
    SDL_DestroyTexture(composition.texture);
}

bool Renderer2D::ClearMaskTarget(SDL_Texture* texture) {
    SDL_Texture* previous = SDL_GetRenderTarget(m_renderer);
    if (!SDL_SetRenderTarget(m_renderer, texture)) return false;
    std::uint8_t red = 0, green = 0, blue = 0, alpha = 0;
    (void)SDL_GetRenderDrawColor(m_renderer, &red, &green, &blue, &alpha);
    const bool clipEnabled = SDL_RenderClipEnabled(m_renderer);
    SDL_Rect clip{};
    if (clipEnabled) (void)SDL_GetRenderClipRect(m_renderer, &clip);
    (void)SDL_SetRenderClipRect(m_renderer, nullptr);
    (void)SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 0);
    const bool cleared = SDL_RenderClear(m_renderer);
    (void)SDL_SetRenderDrawColor(m_renderer, red, green, blue, alpha);
    if (clipEnabled) (void)SDL_SetRenderClipRect(m_renderer, &clip);
    if (!cleared) (void)SDL_SetRenderTarget(m_renderer, previous);
    return cleared;
}

void Renderer2D::DestroyMaskTargets() {
    if (m_maskActive && m_renderer)
        (void)SDL_SetRenderTarget(m_renderer, m_maskPreviousTarget);
    m_maskActive = false;
    m_maskCoverageActive = false;
    m_maskPreviousTarget = nullptr;
    if (m_maskContentTarget) SDL_DestroyTexture(m_maskContentTarget);
    if (m_maskCoverageTarget) SDL_DestroyTexture(m_maskCoverageTarget);
    m_maskContentTarget = nullptr;
    m_maskCoverageTarget = nullptr;
}

bool Renderer2D::BeginAlphaMask() {
    if (!m_renderer || m_maskActive || m_logicalW <= 0 || m_logicalH <= 0)
        return false;
    if (!m_maskContentTarget || !m_maskCoverageTarget) {
        DestroyMaskTargets();
        m_maskContentTarget = SDL_CreateTexture(m_renderer, kTargetFormat,
            SDL_TEXTUREACCESS_TARGET, m_logicalW, m_logicalH);
        m_maskCoverageTarget = SDL_CreateTexture(m_renderer, kTargetFormat,
            SDL_TEXTUREACCESS_TARGET, m_logicalW, m_logicalH);
        if (!m_maskContentTarget || !m_maskCoverageTarget ||
            !SDL_SetTextureBlendMode(m_maskContentTarget, PremultipliedBlendMode()) ||
            !SDL_SetTextureBlendMode(m_maskCoverageTarget, AlphaCoverageBlendMode())) {
            DestroyMaskTargets();
            return false;
        }
        (void)SDL_SetTextureScaleMode(m_maskContentTarget, SDL_SCALEMODE_LINEAR);
        (void)SDL_SetTextureScaleMode(m_maskCoverageTarget, SDL_SCALEMODE_LINEAR);
    }
    SDL_Texture* previous = SDL_GetRenderTarget(m_renderer);
    if (!ClearMaskTarget(m_maskContentTarget)) return false;
    m_maskPreviousTarget = previous;
    m_maskActive = true;
    m_maskCoverageActive = false;
    return true;
}

bool Renderer2D::BeginAlphaMaskCoverage() {
    if (!m_maskActive || m_maskCoverageActive || !m_maskCoverageTarget)
        return false;
    if (!ClearMaskTarget(m_maskCoverageTarget)) return false;
    m_maskCoverageActive = true;
    return true;
}

void Renderer2D::EndAlphaMask(const bool composite, const bool inverted) {
    if (!m_maskActive || !m_renderer) return;
    bool ready = composite && m_maskCoverageActive &&
        SDL_SetTextureBlendMode(m_maskCoverageTarget,
            inverted ? InvertedCoverageBlendMode() : AlphaCoverageBlendMode()) &&
        SDL_SetRenderTarget(m_renderer, m_maskContentTarget);
    if (ready) {
        const bool clipEnabled = SDL_RenderClipEnabled(m_renderer);
        SDL_Rect clip{};
        if (clipEnabled) (void)SDL_GetRenderClipRect(m_renderer, &clip);
        (void)SDL_SetRenderClipRect(m_renderer, nullptr);
        SDL_FRect destination{0, 0, static_cast<float>(m_logicalW),
                              static_cast<float>(m_logicalH)};
        ready = SDL_RenderTexture(m_renderer, m_maskCoverageTarget, nullptr,
                                  &destination);
        if (clipEnabled) (void)SDL_SetRenderClipRect(m_renderer, &clip);
    }
    const bool restored = SDL_SetRenderTarget(m_renderer, m_maskPreviousTarget);
    if (ready && restored) BlitUntransformed(m_maskContentTarget);
    m_maskPreviousTarget = nullptr;
    m_maskCoverageActive = false;
    m_maskActive = false;
}

void Renderer2D::DrawSpriteBatch(
    const std::string& texturePath,
    const std::vector<SpriteBatchItem>& items) {
    if (items.empty()) return;
    SDL_Texture* texture = texturePath.empty()
                               ? nullptr
                               : NativeTexture(m_assets.Texture(texturePath));
    if (!texturePath.empty() && !texture) return;
    float textureWidth = 1.0f;
    float textureHeight = 1.0f;
    if (texture &&
        (!SDL_GetTextureSize(texture, &textureWidth, &textureHeight) ||
         textureWidth <= 0.0f || textureHeight <= 0.0f))
        return;

    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    vertices.reserve(items.size() * 4u);
    indices.reserve(items.size() * 6u);
    for (const auto& item : items) {
        if (item.destination.w <= 0.0f || item.destination.h <= 0.0f ||
            !std::isfinite(item.rotation))
            continue;
        const float centerX = item.destination.x + item.destination.w * 0.5f;
        const float centerY = item.destination.y + item.destination.h * 0.5f;
        const float radians = item.rotation * 0.017453292519943295f;
        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);
        const auto corner = [&](const float x, const float y) {
            const float dx = x - centerX;
            const float dy = y - centerY;
            return TransformPoint({centerX + dx * cosine - dy * sine,
                                   centerY + dx * sine + dy * cosine});
        };
        const auto p0 = corner(item.destination.x, item.destination.y);
        const auto p1 = corner(item.destination.x + item.destination.w,
                               item.destination.y);
        const auto p2 = corner(item.destination.x + item.destination.w,
                               item.destination.y + item.destination.h);
        const auto p3 = corner(item.destination.x,
                               item.destination.y + item.destination.h);
        const SDL_FColor color = ToFColor(TransformColor(item.color));
        float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
        if (texture && item.source.w > 0.0f && item.source.h > 0.0f) {
            const float divisorX = item.sourceNormalized ? 1.0f : textureWidth;
            const float divisorY = item.sourceNormalized ? 1.0f : textureHeight;
            u0 = item.source.x / divisorX;
            v0 = item.source.y / divisorY;
            u1 = (item.source.x + item.source.w) / divisorX;
            v1 = (item.source.y + item.source.h) / divisorY;
        }
        const int base = static_cast<int>(vertices.size());
        vertices.insert(vertices.end(),
                        {{{p0.x, p0.y}, color, {u0, v0}},
                         {{p1.x, p1.y}, color, {u1, v0}},
                         {{p2.x, p2.y}, color, {u1, v1}},
                         {{p3.x, p3.y}, color, {u0, v1}}});
        indices.insert(indices.end(),
                       {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    if (vertices.empty()) return;
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    if (RenderGeometry(texture, vertices.data(),
                           static_cast<int>(vertices.size()), indices.data(),
                           static_cast<int>(indices.size()))) {
        ++m_geometryBatchCount;
        m_geometryBatchItems += vertices.size() / 4u;
    }
}

Rect Renderer2D::DrawImageAuto(const std::string& path, DisplayMode mode, std::uint8_t alpha, int offsetX, int offsetY, float scale, Shadow shadow) {
    const TextureHandle handle = m_assets.Texture(path);
    SDL_Texture* tex = NativeTexture(handle);
    if (!tex) {
        return {};
    }
    int tw = 0, th = 0;
    AssetCache::TextureSize(handle, tw, th);
    if (tw <= 0 || th <= 0) {
        return {};
    }

    const float rw = static_cast<float>(m_logicalW);
    const float rh = static_cast<float>(m_logicalH);
    float w = tw * scale;
    float h = th * scale;
    float x = 0.0f, y = 0.0f;

    switch (mode) {
        case DisplayMode::TopLeft:
            x = 0, y = 0;
            break;
        case DisplayMode::TopRight:
            x = rw - w, y = 0;
            break;
        case DisplayMode::BottomLeft:
            x = 0, y = rh - h;
            break;
        case DisplayMode::BottomRight:
            x = rw - w, y = rh - h;
            break;
        case DisplayMode::Top:
            x = (rw - w) * 0.5f, y = 0;
            break;
        case DisplayMode::Bottom:
            x = (rw - w) * 0.5f, y = rh - h;
            break;
        case DisplayMode::Left:
            x = 0, y = (rh - h) * 0.5f;
            break;
        case DisplayMode::Right:
            x = rw - w, y = (rh - h) * 0.5f;
            break;
        case DisplayMode::Center:
            x = (rw - w) * 0.5f, y = (rh - h) * 0.5f;
            break;
        case DisplayMode::FitWidthBottom: {
            const float s = (rw / tw) * scale;
            w = tw * s, h = th * s, x = 0, y = rh - h;
            break;
        }
        case DisplayMode::Fit: {
            const float s = std::min(rw / tw, rh / th) * scale;
            w = tw * s, h = th * s, x = (rw - w) * 0.5f, y = (rh - h) * 0.5f;
            break;
        }
        case DisplayMode::Fill: {
            const float s = std::max(rw / tw, rh / th) * scale;
            w = tw * s, h = th * s, x = (rw - w) * 0.5f, y = (rh - h) * 0.5f;
            break;
        }
    }

    x += offsetX;
    y += offsetY;
    const Rect dst{ x, y, w, h };

    if (shadow.enabled) {
        SDL_SetTextureColorMod(tex, 0, 0, 0);
        Blit(tex, Rect{ x + shadow.offsetX, y + shadow.offsetY, w, h }, shadow.alpha);
        SDL_SetTextureColorMod(tex, 255, 255, 255);
    }
    Blit(tex, dst, alpha);
    return dst;
}

namespace {
// Text that cannot be drawn would otherwise just be missing on screen.
void WarnTextOnce(const char* what, const std::string& detail) {
    static bool warned = false;
    if (warned) return;
    warned = true;
    PX_LOG_WARN("Renderer2D: text {}: {}", what, detail);
}
}  // namespace

const Renderer2D::CachedText* Renderer2D::AcquireText(const std::string& text, const std::string& fontPath, int size, Color color, int outline, int wrap, HorizontalAlignment alignment, const text::TextOrientation orientation) {
    const int ss = m_effectiveTextSupersample;
    const std::string key = MakeKey(text, fontPath, size, color, outline, wrap, ss,static_cast<int>(alignment),m_textLayout.Locale()) +
                            (orientation == text::TextOrientation::Vertical
                                 ? "|vertical"
                                 : "|horizontal");
    if (auto it = m_textCache.find(key); it != m_textCache.end()) {
        return &it->second;
    }
    // "\r\n" and "\r" break lines like "\n"; tabs draw as spaces and other
    // control characters are left out instead of drawing missing-glyph boxes.
    // A trailing line break starts no visible line.
    std::string drawable = text::MakeDrawable(text);
    while (!drawable.empty() && drawable.back() == '\n') drawable.pop_back();
    if (drawable.empty()) return nullptr;

    // Backlogs and long sessions can otherwise grow the cache without bound.
    if (m_textCache.size() >= kTextCacheLimit) {
        diag::FrameProfiler::Instance().Note("text-cache-clear");
        ClearTextCache();
    }
    diag::FrameProfiler::Instance().Note("text-raster");

    TTF_Font* font = m_assets.Font(fontPath, size * ss, outline * ss);
    if (!font) {
        WarnTextOnce("font unavailable", fontPath);
        return nullptr;
    }
    m_textLayout.ConfigureFont(font, drawable);
    if (orientation == text::TextOrientation::Vertical)
        (void)TTF_SetFontDirection(font, TTF_DIRECTION_TTB);

    const SDL_Color col{color.r,color.g,color.b,color.a};
    const auto wrapAlignment=alignment==HorizontalAlignment::Center?TTF_HORIZONTAL_ALIGN_CENTER:alignment==HorizontalAlignment::Right?TTF_HORIZONTAL_ALIGN_RIGHT:TTF_HORIZONTAL_ALIGN_LEFT;
    TTF_SetFontWrapAlignment(font,wrapAlignment);
    // wrap < 0: the text is already broken into lines; break only at newlines.
    // Unwrapped text with a newline also breaks there: TTF_RenderText_Blended
    // would draw the newline as a glyph.
    const bool lines = wrap != 0 || drawable.find('\n') != std::string::npos;
    SDL_Surface* surface=lines?TTF_RenderText_Blended_Wrapped(font,drawable.c_str(),drawable.size(),col,wrap>0?wrap*ss:0):TTF_RenderText_Blended(font,drawable.c_str(),drawable.size(),col);
    TTF_SetFontWrapAlignment(font,TTF_HORIZONTAL_ALIGN_LEFT);
    if(!surface){WarnTextOnce("rasterizing failed",SDL_GetError());return nullptr;}
    CachedText entry;
#if defined(PRISMATIX_WEB_GL_EFFECTS)
    entry.texture=CreateWebTextureFromSurface(m_renderer,surface);
#else
    entry.texture=SDL_CreateTextureFromSurface(m_renderer,surface);
#endif
    entry.w=surface->w/ss;entry.h=surface->h/ss;SDL_DestroySurface(surface);if(!entry.texture){WarnTextOnce("texture upload failed",SDL_GetError());return nullptr;}SDL_SetTextureBlendMode(entry.texture,SDL_BLENDMODE_BLEND);SDL_SetTextureScaleMode(entry.texture,SDL_SCALEMODE_LINEAR);

    auto [it, _] = m_textCache.emplace(key, entry);
    return &it->second;
}

void Renderer2D::DrawText(const std::string& text, float x, float y, const std::string& fontPath, int size, Color color, std::uint8_t alpha, int wrap) {
    const CachedText* t = AcquireText(text, fontPath, size, color, 0, wrap);
    if (t) {
        Blit(t->texture,{x,y,static_cast<float>(t->w),static_cast<float>(t->h)},alpha);
    }
}

void Renderer2D::DrawTextOutline(const std::string& text, float x, float y, const std::string& fontPath, int size, Color textColor, Color outlineColor, int outlineSize, std::uint8_t alpha, bool shadow, int wrap) {
    if (shadow) {
        const Color black{ 0, 0, 0, 255 };
        const CachedText* s = AcquireText(text, fontPath, size, black, 0, wrap);
        if (s) {
            const auto sa = static_cast<std::uint8_t>(alpha * 0.35f);
            Blit(s->texture,{x+3,y+3,static_cast<float>(s->w),static_cast<float>(s->h)},sa);
        }
    }

    if (outlineSize > 0) {
        const CachedText* o = AcquireText(text, fontPath, size, outlineColor, outlineSize, wrap);
        if (o) {
            Blit(o->texture,{x,y,static_cast<float>(o->w),static_cast<float>(o->h)},alpha);
        }
    }

    const CachedText* f = AcquireText(text, fontPath, size, textColor, 0, wrap);
    if (f) {
        const float ox = static_cast<float>(outlineSize);
        Blit(f->texture,{x+ox,y+ox,static_cast<float>(f->w),static_cast<float>(f->h)},alpha);
    }
}

Vec2 Renderer2D::MeasureText(const std::string& text, const std::string& fontPath, int size, int wrap) {
    return LayoutText(text, fontPath, size, wrap).Size();
}

text::TextLayout Renderer2D::LayoutText(
    const std::string& value, const std::string& fontPath, const int size,
    const int wrap, const text::TextOrientation orientation,
    const std::size_t verticalRows) const {
    return m_textLayout.Layout(value, fontPath, size, wrap, orientation,
                               verticalRows);
}

void Renderer2D::DrawTextLayout(const text::TextLayout& layout,
                                const Vec2 origin,
                                const std::string& fontPath, const int size,
                                const Color color, const std::uint8_t alpha) {
    if (layout.Orientation() == text::TextOrientation::Horizontal) {
        DrawText(layout.Text(), origin.x, origin.y, fontPath, size, color,
                 alpha);
        return;
    }
    for (const auto& cluster : layout.Clusters()) {
        if (cluster.byteStart + cluster.byteLength > layout.Text().size())
            continue;
        const std::string glyph = layout.Text().substr(cluster.byteStart,
                                                       cluster.byteLength);
        const Rect cell{origin.x + cluster.bounds.x,
                        origin.y + cluster.bounds.y, cluster.bounds.w,
                        cluster.bounds.h};
        const CachedText* rendered = AcquireText(
            glyph, fontPath, size, color, 0, 0, HorizontalAlignment::Left,
            cluster.rotated ? text::TextOrientation::Horizontal
                            : text::TextOrientation::Vertical);
        if (!rendered) continue;
        const Vec2 extent{static_cast<float>(rendered->w),
                          static_cast<float>(rendered->h)};
        const Vec2 draw{cell.x + (cell.w - extent.x) * 0.5f,
                        cell.y + (cell.h - extent.y) * 0.5f};
        if (cluster.rotated) {
            const Vec2 center{cell.x + cell.w * 0.5f,
                              cell.y + cell.h * 0.5f};
            PushTransform(center, {1.0f, 1.0f}, 90.0f);
            Blit(rendered->texture,
                 {draw.x, draw.y, extent.x, extent.y}, alpha);
            PopTransform();
        } else {
            Blit(rendered->texture,
                 {draw.x, draw.y, extent.x, extent.y}, alpha);
        }
    }
}

void Renderer2D::SetTextLocale(
    std::string locale, std::vector<std::string> fontFallbackChain) {
    if (locale.empty()) locale = "und";
    const bool localeChanged = m_textLayout.Locale() != locale;
    const bool fontsChanged =
        m_assets.SetFontFallbackChain(std::move(fontFallbackChain));
    if (localeChanged) m_textLayout.SetLocale(std::move(locale));
    if (localeChanged || fontsChanged) ClearTextCache();
}

void Renderer2D::ClearTextCache() {
    m_textLayout.ClearCache();
    m_opticalOffsets.clear();
    for (auto& [key, entry] : m_textCache) {
        if (entry.texture) SDL_DestroyTexture(entry.texture);
    }
    m_textCache.clear();
}

void Renderer2D::SetTextSupersample(int factor) {
    factor = std::clamp(factor, 1, 4);
    m_textSamplingMode=TextSamplingMode::Fixed;m_fixedTextSupersample=factor;
    if (factor == m_effectiveTextSupersample) {
        return;
    }
    m_effectiveTextSupersample = factor;
    ClearTextCache();
}

namespace {
// CJK ideographs, kana, Hangul and fullwidth forms take the ideographic
// reference; everything below U+2E80 (Latin, digits, Greek, Cyrillic,
// punctuation) the capital one.
bool HasIdeographicText(const std::string_view text) {
    for (std::size_t index = 0; index < text.size();) {
        const auto lead = static_cast<unsigned char>(text[index]);
        if (lead < 0x80) { ++index; continue; }
        const std::size_t length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (index + length > text.size()) break;
        std::uint32_t code = length == 4 ? lead & 0x07u : length == 3 ? lead & 0x0Fu : lead & 0x1Fu;
        for (std::size_t next = 1; next < length; ++next)
            code = (code << 6) | (static_cast<unsigned char>(text[index + next]) & 0x3Fu);
        // Variation selectors ride on the letter before them.
        if (code >= 0x2E80 && !(code >= 0xFE00 && code <= 0xFE0F)) return true;
        index += length;
    }
    return false;
}
}  // namespace

float Renderer2D::OpticalCenterOffset(const std::string_view text, const std::string& fontPath,
                                      const int size) {
    if (size <= 0) return 0.0f;
    const bool ideographic = HasIdeographicText(text);
    const int ss = m_effectiveTextSupersample;
    std::string key = fontPath;
    key += '|';
    key += std::to_string(size);
    key += '|';
    key += std::to_string(ss);
    key += ideographic ? "|i" : "|c";
    if (const auto found = m_opticalOffsets.find(key); found != m_opticalOffsets.end())
        return found->second;
    float offset = 0.0f;
    // The font the text is rasterised with (fallbacks included), at the same
    // supersampled size, so the metrics are those of the drawn pixels.
    if (TTF_Font* font = m_assets.Font(fontPath, size * ss, 0)) {
        int minY = 0, maxY = 0;
        bool measured = ideographic &&
            TTF_GetGlyphMetrics(font, 0x4E2D, nullptr, nullptr, &minY, &maxY, nullptr) && maxY > minY;
        if (!measured)
            measured = TTF_GetGlyphMetrics(font, 'H', nullptr, nullptr, &minY, &maxY, nullptr) && maxY > minY;
        if (measured) {
            // Rows from the top of the line box: the baseline is `ascent`
            // down; the glyph's ink spans minY..maxY above it.
            const float ascent = static_cast<float>(TTF_GetFontAscent(font));
            const float lineBox = static_cast<float>(TTF_GetFontHeight(font));
            const float ink = ascent - (static_cast<float>(minY) + static_cast<float>(maxY)) * 0.5f;
            offset = (lineBox * 0.5f - ink) / static_cast<float>(ss);
            // A font with nonsensical metrics must not throw text away.
            const float limit = static_cast<float>(size) * 0.25f;
            offset = std::clamp(offset, -limit, limit);
        }
    }
    m_opticalOffsets.emplace(std::move(key), offset);
    return offset;
}

void Renderer2D::DrawTextInRect(const std::string& text, const Rect& bounds,
                                const std::string& fontPath, int size, Color color,
                                HorizontalAlignment horizontal, VerticalAlignment vertical,
                                bool wrap, std::uint8_t alpha,
                                int outlineSize, Color outlineColor, bool newlinesOnly,
                                const std::string_view opticalReference) {
    const int wrapWidth=newlinesOnly?-1:wrap?std::max(1,static_cast<int>(std::floor(bounds.w))):0;
    // Centred text is centred on its letters (see OpticalCenterOffset); Top
    // and Bottom keep the line box flush with the edge.
    const float optical=vertical==VerticalAlignment::Center
        ?OpticalCenterOffset(opticalReference.empty()?std::string_view(text):opticalReference,fontPath,size):0.0f;
    const CachedText* t=AcquireText(text,fontPath,size,color,0,wrapWidth,horizontal);if(!t)return;
    float x=bounds.x,y=bounds.y;
    if(horizontal==HorizontalAlignment::Center)x+=(bounds.w-t->w)*.5f;else if(horizontal==HorizontalAlignment::Right)x+=bounds.w-t->w;
    if(vertical==VerticalAlignment::Center)y+=(bounds.h-t->h)*.5f+optical;else if(vertical==VerticalAlignment::Bottom)y+=bounds.h-t->h;
    const int edge=std::clamp(outlineSize,0,8);
    const float bleed=static_cast<float>(edge);
    // The optical shift may move the line box past a tight rectangle's edge;
    // the clip follows it so nothing that showed before is cut.
    const float above=bleed+std::max(0.0f,-optical),below=bleed+std::max(0.0f,optical);
    PushClip({bounds.x-bleed,bounds.y-above,bounds.w+bleed*2.0f,bounds.h+above+below});
    if(edge>0){
        // SDL_ttf wraps outlined glyphs using their enlarged metrics. During
        // typewriter reveal this can move the edge to the next line before the
        // fill, making the shadow appear to jump. Use the identical unoutlined
        // glyph layout for both passes and build the edge with small offsets.
        const CachedText* outline=AcquireText(text,fontPath,size,outlineColor,0,wrapWidth,horizontal);
        if(outline){
            for(int dy=-edge;dy<=edge;++dy)
                for(int dx=-edge;dx<=edge;++dx){
                    if((dx==0&&dy==0)||dx*dx+dy*dy>edge*edge)continue;
                    Blit(outline->texture,{x+static_cast<float>(dx),y+static_cast<float>(dy),
                        static_cast<float>(outline->w),static_cast<float>(outline->h)},alpha);
                }
        }
    }
    // Acquiring the edge can evict the text cache. Reacquire the fill after
    // drawing the edge rather than retaining a potentially invalid pointer.
    if(const CachedText* fill=AcquireText(text,fontPath,size,color,0,wrapWidth,horizontal))
        Blit(fill->texture,{x,y,static_cast<float>(fill->w),static_cast<float>(fill->h)},alpha);
    PopClip();
}

void Renderer2D::SetTextSupersampleAuto(){if(m_textSamplingMode==TextSamplingMode::Auto)return;m_textSamplingMode=TextSamplingMode::Auto;SetLogicalSize(m_logicalW,m_logicalH);}

// Raw extension drawing ------------------------------------------------------

namespace {

SDL_BlendMode RawNativeBlend(const RawBlendMode mode) {
    switch (mode) {
    case RawBlendMode::Premultiplied: return PremultipliedBlendMode();
    case RawBlendMode::Additive: return SDL_BLENDMODE_ADD;
    case RawBlendMode::Multiply: return SDL_BLENDMODE_MUL;
    case RawBlendMode::Replace: return SDL_BLENDMODE_NONE;
    case RawBlendMode::Normal: break;
    }
    return SDL_BLENDMODE_BLEND;
}

}  // namespace

TextureResource Renderer2D::CreateRawTexture(const int width, const int height,
                                             const bool linear) {
    if (!m_renderer || width <= 0 || height <= 0) return {};
    auto texture = TextureResource::CreateStreaming(
        TextureBackend::SdlRenderer, m_renderer, StreamingTextureFormat::Rgba32,
        width, height);
    if (!texture) return {};
    (void)texture.SetLinearSampling(linear);
    (void)texture.SetAlphaBlend(true);
    // A new streaming texture's contents are undefined: start transparent.
    const std::vector<std::uint8_t> clear(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0);
    (void)texture.Update(clear.data(), static_cast<std::size_t>(width) * 4u);
    return texture;
}

TextureResource Renderer2D::CreateRawTarget(const int width, const int height,
                                            const bool linear) {
    if (!m_renderer || width <= 0 || height <= 0) return {};
    SDL_Texture* native = SDL_CreateTexture(m_renderer, kTargetFormat,
                                            SDL_TEXTUREACCESS_TARGET, width, height);
    auto texture = TextureResource::Adopt(TextureBackend::SdlRenderer, native,
                                          width, height);
    if (!texture) return {};
    (void)texture.SetLinearSampling(linear);
    (void)SDL_SetTextureBlendMode(native, PremultipliedBlendMode());
    SDL_Texture* previous = SDL_GetRenderTarget(m_renderer);
    if (SDL_SetRenderTarget(m_renderer, native)) {
        (void)SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 0);
        (void)SDL_RenderClear(m_renderer);
    }
    (void)SDL_SetRenderTarget(m_renderer, previous);
    return texture;
}

bool Renderer2D::UpdateRawTexture(TextureResource& texture, const Rect& region,
                                  const std::uint8_t* pixels,
                                  const std::size_t pitchBytes) {
    auto* native = static_cast<SDL_Texture*>(
        texture.Native(TextureBackend::SdlRenderer));
    if (!native || !pixels || pitchBytes == 0 ||
        pitchBytes > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return false;
    const SDL_Rect rect{static_cast<int>(region.x), static_cast<int>(region.y),
                        static_cast<int>(region.w), static_cast<int>(region.h)};
    if (rect.x < 0 || rect.y < 0 || rect.w <= 0 || rect.h <= 0 ||
        rect.x > texture.Handle().Width() - rect.w ||
        rect.y > texture.Handle().Height() - rect.h)
        return false;
    return SDL_UpdateTexture(native, &rect, pixels, static_cast<int>(pitchBytes));
}

bool Renderer2D::BeginRawTarget(const TextureHandle target,
                                const std::optional<Color> clear) {
    auto* native = NativeTexture(target);
    if (!m_renderer || !native || m_rawTarget) return false;
    SDL_Texture* previous = SDL_GetRenderTarget(m_renderer);
    if (!SDL_SetRenderTarget(m_renderer, native)) return false;
    // SDL keeps viewport and clip per target; the renderer's own transform,
    // camera and clip stack describe the screen, so they are set aside.
    m_rawTarget = RawTargetState{previous, std::move(m_transformStack),
                                 std::move(m_clipStack)};
    m_transformStack = {TransformState{}};
    m_clipStack.clear();
    std::swap(m_camX, m_rawTargetCamera.first);
    std::swap(m_camY, m_rawTargetCamera.second);
    (void)SDL_SetRenderClipRect(m_renderer, nullptr);
    if (clear) {
        (void)SDL_SetRenderDrawColor(m_renderer, clear->r, clear->g, clear->b,
                                     clear->a);
        (void)SDL_RenderClear(m_renderer);
    }
    return true;
}

void Renderer2D::EndRawTarget() {
    if (!m_rawTarget) return;
    (void)SDL_SetRenderTarget(m_renderer, m_rawTarget->previousTarget);
    m_transformStack = std::move(m_rawTarget->transforms);
    m_clipStack = std::move(m_rawTarget->clips);
    std::swap(m_camX, m_rawTargetCamera.first);
    std::swap(m_camY, m_rawTargetCamera.second);
    m_rawTarget.reset();
}

void Renderer2D::DrawRawGeometry(const TextureHandle texture,
                                 const RawVertex* vertices, const int vertexCount,
                                 const int* indices, const int indexCount,
                                 const RawBlendMode blend,
                                 const AffineTransform2D& transform) {
    if (!m_renderer || !vertices || vertexCount <= 0) return;
    SDL_Texture* native = NativeTexture(texture);
    if (texture && !native) return;
    const Color modulate = TransformColor({255, 255, 255, 255});
    const float mr = modulate.r / 255.0f, mg = modulate.g / 255.0f,
                mb = modulate.b / 255.0f, ma = modulate.a / 255.0f;
    std::vector<SDL_Vertex> converted(static_cast<std::size_t>(vertexCount));
    for (int index = 0; index < vertexCount; ++index) {
        const auto& source = vertices[index];
        const Vec2 local{transform.a * source.x + transform.c * source.y + transform.tx,
                         transform.b * source.x + transform.d * source.y + transform.ty};
        const Vec2 point = TransformPoint(local);
        auto& out = converted[static_cast<std::size_t>(index)];
        out.position = SDL_FPoint{point.x, point.y};
        out.color = SDL_FColor{std::clamp(source.r, 0.0f, 1.0f) * mr,
                               std::clamp(source.g, 0.0f, 1.0f) * mg,
                               std::clamp(source.b, 0.0f, 1.0f) * mb,
                               std::clamp(source.a, 0.0f, 1.0f) * ma};
        out.tex_coord = SDL_FPoint{source.u, source.v};
    }
    const SDL_BlendMode mode = RawNativeBlend(blend);
    if (native) {
        SDL_BlendMode previous = SDL_BLENDMODE_BLEND;
        if (!SDL_GetTextureBlendMode(native, &previous) ||
            !SDL_SetTextureBlendMode(native, mode))
            return;
        (void)RenderGeometry(native, converted.data(), vertexCount, indices,
                             indexCount);
        (void)SDL_SetTextureBlendMode(native, previous);
    } else {
        (void)SDL_SetRenderDrawBlendMode(m_renderer, mode);
        (void)RenderGeometry(nullptr, converted.data(), vertexCount, indices,
                             indexCount);
        (void)SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    }
    ++m_geometryBatchCount;
    m_geometryBatchItems += static_cast<std::uint64_t>(vertexCount);
}

std::string Renderer2D::BackendName() const {
    const char* name = m_renderer ? SDL_GetRendererName(m_renderer) : nullptr;
    return name ? name : "none";
}

}  // namespace px::graphics
