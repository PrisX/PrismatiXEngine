#include "Engine/Graphics/Renderer2D.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace px::graphics {

namespace {
constexpr std::size_t kTextCacheLimit = 600;

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

std::string MakeKey(const std::string& text, const std::string& font, int size, Color c, int outline, int wrap, int ss) {
    return text + "\x01" + font + "\x01" + std::to_string(size) + "\x01" + std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + "," + std::to_string(c.a) + "\x01" + std::to_string(outline) + "\x01" + std::to_string(wrap) + "\x01" +
           std::to_string(ss);
}
}  // namespace

Renderer2D::Renderer2D(SDL_Renderer* renderer, AssetCache& assets)
    : m_renderer(renderer), m_textEngine(TTF_CreateRendererTextEngine(renderer)), m_assets(assets) {}

Renderer2D::~Renderer2D() {
    ClearTextCache();
    if (m_textEngine) TTF_DestroyRendererTextEngine(m_textEngine);
}

void Renderer2D::SetLogicalSize(int width, int height) {
    m_logicalW = width;
    m_logicalH = height;
    SDL_SetRenderLogicalPresentation(m_renderer, width, height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

void Renderer2D::GetLogicalSize(int& width, int& height) const {
    width = m_logicalW;
    height = m_logicalH;
}

void Renderer2D::DrawRect(const Rect& rect, Color color) {
    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(m_renderer, color.r, color.g, color.b, color.a);
    SDL_FRect r{ rect.x + m_camX, rect.y + m_camY, rect.w, rect.h };
    SDL_RenderFillRect(m_renderer, &r);
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

    const SDL_FColor fc = ToFColor(color);
    const float left = rect.x + m_camX;
    const float top = rect.y + m_camY;
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

    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(m_renderer, nullptr, fan.data(), static_cast<int>(fan.size()), nullptr, 0);
}

void Renderer2D::PushClip(const Rect& requested) {
    Rect clip = requested;
    if (!m_clipStack.empty()) {
        const Rect parent = m_clipStack.back();
        const float left = std::max(parent.x, clip.x);
        const float top = std::max(parent.y, clip.y);
        const float right = std::min(parent.x + parent.w, clip.x + clip.w);
        const float bottom = std::min(parent.y + parent.h, clip.y + clip.h);
        clip = {left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top)};
    }
    m_clipStack.push_back(clip);
    const SDL_Rect value{static_cast<int>(clip.x), static_cast<int>(clip.y),
                         static_cast<int>(clip.w), static_cast<int>(clip.h)};
    SDL_SetRenderClipRect(m_renderer, &value);
}

void Renderer2D::PopClip() {
    if (m_clipStack.empty()) return;
    m_clipStack.pop_back();
    if (m_clipStack.empty()) {
        SDL_SetRenderClipRect(m_renderer, nullptr);
    } else {
        const Rect clip = m_clipStack.back();
        const SDL_Rect value{static_cast<int>(clip.x), static_cast<int>(clip.y),
                             static_cast<int>(clip.w), static_cast<int>(clip.h)};
        SDL_SetRenderClipRect(m_renderer, &value);
    }
}

void Renderer2D::Blit(SDL_Texture* texture, const Rect& dst, std::uint8_t alpha) {
    if (!texture) {
        return;
    }
    SDL_SetTextureAlphaMod(texture, alpha);
    SDL_FRect d{ dst.x + m_camX, dst.y + m_camY, dst.w, dst.h };
    SDL_RenderTexture(m_renderer, texture, nullptr, &d);
    SDL_SetTextureAlphaMod(texture, 255);
}

void Renderer2D::DrawImage(const std::string& path, const Rect& dst, std::uint8_t alpha) { Blit(m_assets.Texture(path), dst, alpha); }

void Renderer2D::DrawTexture(SDL_Texture* texture, const Rect& dst, std::uint8_t alpha) { Blit(texture, dst, alpha); }

Rect Renderer2D::DrawImageAuto(const std::string& path, DisplayMode mode, std::uint8_t alpha, int offsetX, int offsetY, float scale, Shadow shadow) {
    SDL_Texture* tex = m_assets.Texture(path);
    if (!tex) {
        return {};
    }
    int tw = 0, th = 0;
    AssetCache::TextureSize(tex, tw, th);
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

const Renderer2D::CachedText* Renderer2D::AcquireText(const std::string& text, const std::string& fontPath, int size, Color color, int outline, int wrap) {
    const int ss = 1; // SDL_ttf's renderer text engine maintains a shaped glyph atlas.
    const std::string key = MakeKey(text, fontPath, size, color, outline, wrap, ss);
    if (auto it = m_textCache.find(key); it != m_textCache.end()) {
        return &it->second;
    }

    // Backlogs and long sessions can otherwise grow the cache without bound.
    if (m_textCache.size() >= kTextCacheLimit) {
        ClearTextCache();
    }

    TTF_Font* font = m_assets.Font(fontPath, size * ss, outline * ss);
    if (!font || text.empty()) {
        return nullptr;
    }

    if (!m_textEngine) {
        return nullptr;
    }

    CachedText entry;
    entry.text = TTF_CreateText(m_textEngine, font, text.c_str(), text.size());
    if (!entry.text) {
        return nullptr;
    }
    TTF_SetTextColor(entry.text, color.r, color.g, color.b, color.a);
    if (wrap > 0) TTF_SetTextWrapWidth(entry.text, wrap);
    if (!TTF_GetTextSize(entry.text, &entry.w, &entry.h)) {
        TTF_DestroyText(entry.text);
        return nullptr;
    }

    auto [it, _] = m_textCache.emplace(key, entry);
    return &it->second;
}

void Renderer2D::DrawText(const std::string& text, float x, float y, const std::string& fontPath, int size, Color color, std::uint8_t alpha, int wrap) {
    const CachedText* t = AcquireText(text, fontPath, size, color, 0, wrap);
    if (t) {
        TTF_SetTextColor(t->text, color.r, color.g, color.b,
                         static_cast<std::uint8_t>((static_cast<unsigned>(color.a) * alpha) / 255));
        TTF_DrawRendererText(t->text, x + m_camX, y + m_camY);
    }
}

void Renderer2D::DrawTextOutline(const std::string& text, float x, float y, const std::string& fontPath, int size, Color textColor, Color outlineColor, int outlineSize, std::uint8_t alpha, bool shadow, int wrap) {
    if (shadow) {
        const Color black{ 0, 0, 0, 255 };
        const CachedText* s = AcquireText(text, fontPath, size, black, 0, wrap);
        if (s) {
            const auto sa = static_cast<std::uint8_t>(alpha * 0.35f);
            TTF_SetTextColor(s->text, 0, 0, 0, sa);
            TTF_DrawRendererText(s->text, x + 3 + m_camX, y + 3 + m_camY);
        }
    }

    if (outlineSize > 0) {
        const CachedText* o = AcquireText(text, fontPath, size, outlineColor, outlineSize, wrap);
        if (o) {
            TTF_SetTextColor(o->text, outlineColor.r, outlineColor.g, outlineColor.b, alpha);
            TTF_DrawRendererText(o->text, x + m_camX, y + m_camY);
        }
    }

    const CachedText* f = AcquireText(text, fontPath, size, textColor, 0, wrap);
    if (f) {
        const float ox = static_cast<float>(outlineSize);
        TTF_SetTextColor(f->text, textColor.r, textColor.g, textColor.b, alpha);
        TTF_DrawRendererText(f->text, x + ox + m_camX, y + ox + m_camY);
    }
}

Vec2 Renderer2D::MeasureText(const std::string& text, const std::string& fontPath, int size, int wrap) {
    TTF_Font* font = m_assets.Font(fontPath, size, 0);
    if (!font) {
        return {};
    }
    int w = 0, h = 0;
    if (wrap > 0) {
        TTF_GetStringSizeWrapped(font, text.c_str(), text.size(), wrap, &w, &h);
    }
    else {
        TTF_GetStringSize(font, text.c_str(), text.size(), &w, &h);
    }
    return Vec2{ static_cast<float>(w), static_cast<float>(h) };
}

void Renderer2D::ClearTextCache() {
    for (auto& [key, entry] : m_textCache) {
        if (entry.text) TTF_DestroyText(entry.text);
    }
    m_textCache.clear();
}

void Renderer2D::SetTextSupersample(int factor) {
    factor = std::clamp(factor, 1, 4);
    if (factor == m_textSupersample) {
        return;
    }
    m_textSupersample = factor;
    ClearTextCache();
}

}  // namespace px::graphics
