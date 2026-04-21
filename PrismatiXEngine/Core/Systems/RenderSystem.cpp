#include "RenderSystem.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Core/EngineConfig.h"
#include "Core/Services/ResourceManager.h"

namespace PrismatiX::Systems {

namespace {
void AppendArc(std::vector<SDL_Vertex>& vertices, float centerX, float centerY, float radius, float startAngle, float endAngle, const SDL_Color& color, int segments) {
    for (int index = 0; index <= segments; ++index) {
        const float t = static_cast<float>(index) / static_cast<float>(std::max(segments, 1));
        const float angle = startAngle + (endAngle - startAngle) * t;
        SDL_Vertex vertex{};
        vertex.position.x = centerX + std::cos(angle) * radius;
        vertex.position.y = centerY + std::sin(angle) * radius;
        vertex.color = color;
        vertices.push_back(vertex);
    }
}
}  // namespace

RenderSystem::RenderSystem(SDL_Renderer* ren, PrismatiX::Services::ResourceManager& resMgr)
    : renderer(ren), resourceManager(resMgr), textCache(200, [](CachedTexture ct) {
          if (ct.texture) SDL_DestroyTexture(ct.texture);
      }) {}

RenderSystem::~RenderSystem() { textCache.Clear(); }

void RenderSystem::DrawTexture(SDL_Texture* tex, int x, int y, float scale) {
    if (!tex || !renderer) return;
    int originalWidth = 0, originalHeight = 0;
    SDL_QueryTexture(tex, NULL, NULL, &originalWidth, &originalHeight);
    SDL_Rect destRect = { x + cameraOffsetX, y + cameraOffsetY, static_cast<int>(originalWidth * scale), static_cast<int>(originalHeight * scale) };
    SDL_RenderCopy(renderer, tex, NULL, &destRect);
}

void RenderSystem::DrawTexture(SDL_Texture* tex, int x, int y, int w, int h, Uint8 alpha) {
    if (!tex || !renderer) return;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(tex, alpha);
    SDL_Rect destRect = { x + cameraOffsetX, y + cameraOffsetY, w, h };
    SDL_RenderCopy(renderer, tex, NULL, &destRect);
    SDL_SetTextureAlphaMod(tex, 255);
}

SDL_Rect RenderSystem::DrawTextureAuto(SDL_Texture* tex, DisplayMode mode, Uint8 alpha, int offsetX, int offsetY, float scale, ShadowConfig shadow) {
    if (!tex || !renderer) return SDL_Rect{ 0, 0, 0, 0 };
    int texW = 0, texH = 0;
    if (SDL_QueryTexture(tex, NULL, NULL, &texW, &texH) != 0 || texW <= 0 || texH <= 0) return SDL_Rect{ 0, 0, 0, 0 };
    int renderW = 0, renderH = 0;
    if (SDL_Texture* renderTarget = SDL_GetRenderTarget(renderer)) {
        SDL_QueryTexture(renderTarget, nullptr, nullptr, &renderW, &renderH);
    } else {
        SDL_RenderGetLogicalSize(renderer, &renderW, &renderH);
        if (renderW <= 0 || renderH <= 0) {
            if (SDL_GetRendererOutputSize(renderer, &renderW, &renderH) != 0 || renderW <= 0 || renderH <= 0) return SDL_Rect{ 0, 0, 0, 0 };
        }
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(tex, alpha);
    int scaledW = std::max(1, static_cast<int>(texW * scale));
    int scaledH = std::max(1, static_cast<int>(texH * scale));
    SDL_Rect destRect = { 0, 0, scaledW, scaledH };

    switch (mode) {
        case DisplayMode::TopLeft:
            destRect.x = 0;
            destRect.y = 0;
            break;
        case DisplayMode::TopRight:
            destRect.x = renderW - scaledW;
            destRect.y = 0;
            break;
        case DisplayMode::BottomLeft:
            destRect.x = 0;
            destRect.y = renderH - scaledH;
            break;
        case DisplayMode::BottomRight:
            destRect.x = renderW - scaledW;
            destRect.y = renderH - scaledH;
            break;
        case DisplayMode::Top:
            destRect.x = (renderW - scaledW) / 2;
            destRect.y = 0;
            break;
        case DisplayMode::Bottom:
            destRect.x = (renderW - scaledW) / 2;
            destRect.y = renderH - scaledH;
            break;
        case DisplayMode::Left:
            destRect.x = 0;
            destRect.y = (renderH - scaledH) / 2;
            break;
        case DisplayMode::Right:
            destRect.x = renderW - scaledW;
            destRect.y = (renderH - scaledH) / 2;
            break;
        case DisplayMode::Center:
            destRect.x = (renderW - scaledW) / 2;
            destRect.y = (renderH - scaledH) / 2;
            break;
        case DisplayMode::FitWidthBottom: {
            float fitScale = static_cast<float>(renderW) / texW * scale;
            destRect.w = std::max(1, static_cast<int>(texW * fitScale));
            destRect.h = std::max(1, static_cast<int>(texH * fitScale));
            destRect.x = 0;
            destRect.y = renderH - destRect.h;
            break;
        }
        case DisplayMode::Fit: {
            float fitScale = std::min(static_cast<float>(renderW) / texW, static_cast<float>(renderH) / texH) * scale;
            destRect.w = std::max(1, static_cast<int>(texW * fitScale));
            destRect.h = std::max(1, static_cast<int>(texH * fitScale));
            destRect.x = (renderW - destRect.w) / 2;
            destRect.y = (renderH - destRect.h) / 2;
            break;
        }
        case DisplayMode::Fill: {
            float fitScale = std::max(static_cast<float>(renderW) / texW, static_cast<float>(renderH) / texH) * scale;
            destRect.w = std::max(1, static_cast<int>(texW * fitScale));
            destRect.h = std::max(1, static_cast<int>(texH * fitScale));
            destRect.x = (renderW - destRect.w) / 2;
            destRect.y = (renderH - destRect.h) / 2;
            break;
        }
    }
    destRect.x += offsetX + cameraOffsetX;
    destRect.y += offsetY + cameraOffsetY;
    if (shadow.enabled) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureColorMod(tex, 0, 0, 0);
        Uint8 outerAlpha = (Uint8)(shadow.alpha * 0.45f);
        SDL_SetTextureAlphaMod(tex, outerAlpha);
        const int outerOffsets[4][2] = { { shadow.offsetX - 1, shadow.offsetY - 1 }, { shadow.offsetX + 1, shadow.offsetY - 1 }, { shadow.offsetX - 1, shadow.offsetY + 1 }, { shadow.offsetX + 1, shadow.offsetY + 1 } };
        for (const auto& off : outerOffsets) {
            SDL_Rect shadowRect = { destRect.x + off[0], destRect.y + off[1], destRect.w, destRect.h };
            SDL_RenderCopy(renderer, tex, NULL, &shadowRect);
        }
        SDL_SetTextureAlphaMod(tex, shadow.alpha);
        SDL_Rect coreRect = { destRect.x + shadow.offsetX, destRect.y + shadow.offsetY, destRect.w, destRect.h };
        SDL_RenderCopy(renderer, tex, NULL, &coreRect);
        SDL_SetTextureColorMod(tex, 255, 255, 255);
    }
    SDL_SetTextureAlphaMod(tex, alpha);
    SDL_RenderCopy(renderer, tex, NULL, &destRect);
    SDL_SetTextureAlphaMod(tex, 255);
    return destRect;
}

void RenderSystem::DrawRoundedRect(float x, float y, float w, float h, float radius, SDL_Color color) {
    if (!renderer || w <= 0.0f || h <= 0.0f) {
        return;
    }

    const float clampedRadius = std::clamp(radius, 0.0f, std::min(w, h) * 0.5f);
    if (clampedRadius <= 0.5f) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        SDL_Rect rect{
            static_cast<int>(std::round(x)) + cameraOffsetX,
            static_cast<int>(std::round(y)) + cameraOffsetY,
            static_cast<int>(std::round(w)),
            static_cast<int>(std::round(h)),
        };
        SDL_RenderFillRect(renderer, &rect);
        return;
    }

    std::vector<SDL_Vertex> polygon;
    polygon.reserve(40);

    const float left = x + cameraOffsetX;
    const float top = y + cameraOffsetY;
    const float right = left + w;
    const float bottom = top + h;
    const float pi = 3.14159265358979323846f;
    const int arcSegments = 6;

    AppendArc(polygon, right - clampedRadius, top + clampedRadius, clampedRadius, -pi * 0.5f, 0.0f, color, arcSegments);
    AppendArc(polygon, right - clampedRadius, bottom - clampedRadius, clampedRadius, 0.0f, pi * 0.5f, color, arcSegments);
    AppendArc(polygon, left + clampedRadius, bottom - clampedRadius, clampedRadius, pi * 0.5f, pi, color, arcSegments);
    AppendArc(polygon, left + clampedRadius, top + clampedRadius, clampedRadius, pi, pi * 1.5f, color, arcSegments);

    SDL_Vertex center{};
    center.position.x = left + w * 0.5f;
    center.position.y = top + h * 0.5f;
    center.color = color;

    std::vector<SDL_Vertex> fan;
    fan.reserve(polygon.size() * 3);
    for (size_t index = 0; index < polygon.size(); ++index) {
        fan.push_back(center);
        fan.push_back(polygon[index]);
        fan.push_back(polygon[(index + 1) % polygon.size()]);
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(renderer, nullptr, fan.data(), static_cast<int>(fan.size()), nullptr, 0);
}

SDL_Surface* RenderSystem::RenderTextSurface(TTF_Font* font, const std::string& text, SDL_Color color, Uint32 wrapLength) {
    if (wrapLength > 0) return TTF_RenderUTF8_Blended_Wrapped(font, text.c_str(), color, wrapLength * EngineConfig::kFontOversample);
    return TTF_RenderUTF8_Blended(font, text.c_str(), color);
}

void RenderSystem::DrawText(TTF_Font* font, const std::string& text, SDL_Color color, int x, int y) {
    if (!font || !renderer || text.empty()) return;

    TextCacheKey key{ text, font, color, { 0, 0, 0, 0 }, 0, 0 };
    CachedTexture ct;
    if (!textCache.Get(key, ct)) {
        SDL_Surface* surfaceMessage = TTF_RenderUTF8_Blended(font, text.c_str(), color);
        if (!surfaceMessage) return;
        ct.texture = SDL_CreateTextureFromSurface(renderer, surfaceMessage);
        ct.w = surfaceMessage->w;
        ct.h = surfaceMessage->h;
        SDL_FreeSurface(surfaceMessage);
        textCache.Put(key, ct);
    }

    SDL_Rect messageRect = { x + cameraOffsetX, y + cameraOffsetY, ct.w / EngineConfig::kFontOversample, ct.h / EngineConfig::kFontOversample };
    SDL_RenderCopy(renderer, ct.texture, NULL, &messageRect);
}

void RenderSystem::DrawTextWithOutline(TTF_Font* font, const std::string& text, SDL_Color textColor, SDL_Color outlineColor, int outlineSize, int x, int y, Uint32 wrapLength, Uint8 alpha, bool shadow) {
    if (!font || text.empty() || !renderer) return;
    int renderX = x + cameraOffsetX;
    int renderY = y + cameraOffsetY;

    if (shadow) {
        const int shadowDX = 3, shadowDY = 3;
        SDL_Color black = { 0, 0, 0, 255 };
        TextCacheKey sKey{ text, font, black, { 0, 0, 0, 0 }, -1, wrapLength };  // -1 for shadow
        CachedTexture sct;
        if (!textCache.Get(sKey, sct)) {
            SDL_Surface* sSurf = RenderTextSurface(font, text, black, wrapLength);
            if (sSurf) {
                sct.texture = SDL_CreateTextureFromSurface(renderer, sSurf);
                sct.w = sSurf->w;
                sct.h = sSurf->h;
                SDL_FreeSurface(sSurf);
                textCache.Put(sKey, sct);
            }
        }
        if (sct.texture) {
            int sw = sct.w / EngineConfig::kFontOversample, sh = sct.h / EngineConfig::kFontOversample;
            SDL_SetTextureBlendMode(sct.texture, SDL_BLENDMODE_BLEND);
            const int outerOffsets[4][2] = { { shadowDX - 1, shadowDY - 1 }, { shadowDX + 1, shadowDY - 1 }, { shadowDX - 1, shadowDY + 1 }, { shadowDX + 1, shadowDY + 1 } };
            SDL_SetTextureAlphaMod(sct.texture, (Uint8)(alpha * 0.15f));
            for (const auto& off : outerOffsets) {
                SDL_Rect r = { renderX + off[0], renderY + off[1], sw, sh };
                SDL_RenderCopy(renderer, sct.texture, NULL, &r);
            }
            SDL_SetTextureAlphaMod(sct.texture, (Uint8)(alpha * 0.4f));
            SDL_Rect coreRect = { renderX + shadowDX, renderY + shadowDY, sw, sh };
            SDL_RenderCopy(renderer, sct.texture, NULL, &coreRect);
        }
    }

    CachedTexture bgct = { nullptr, 0, 0 };
    TTF_Font* outlineFont = resourceManager.GetOutlineFont(font, outlineSize);
    if (outlineFont) {
        TextCacheKey bgKey{ text, outlineFont, outlineColor, { 0, 0, 0, 0 }, outlineSize, wrapLength };
        if (!textCache.Get(bgKey, bgct)) {
            SDL_Surface* bgSurf = RenderTextSurface(outlineFont, text, outlineColor, wrapLength);
            if (bgSurf) {
                bgct.texture = SDL_CreateTextureFromSurface(renderer, bgSurf);
                bgct.w = bgSurf->w;
                bgct.h = bgSurf->h;
                SDL_FreeSurface(bgSurf);
                textCache.Put(bgKey, bgct);
            }
        }
    }

    TextCacheKey fgKey{ text, font, textColor, { 0, 0, 0, 0 }, 0, wrapLength };
    CachedTexture fgct;
    if (!textCache.Get(fgKey, fgct)) {
        SDL_Surface* fgSurf = RenderTextSurface(font, text, textColor, wrapLength);
        if (fgSurf) {
            fgct.texture = SDL_CreateTextureFromSurface(renderer, fgSurf);
            fgct.w = fgSurf->w;
            fgct.h = fgSurf->h;
            SDL_FreeSurface(fgSurf);
            textCache.Put(fgKey, fgct);
        }
    }

    if (bgct.texture) {
        SDL_SetTextureAlphaMod(bgct.texture, alpha);
        SDL_Rect bgRect = { renderX, renderY, bgct.w / EngineConfig::kFontOversample, bgct.h / EngineConfig::kFontOversample };
        SDL_RenderCopy(renderer, bgct.texture, NULL, &bgRect);
    }
    if (fgct.texture) {
        SDL_SetTextureAlphaMod(fgct.texture, alpha);
        SDL_Rect fgRect = { renderX + outlineSize, renderY + outlineSize, fgct.w / EngineConfig::kFontOversample, fgct.h / EngineConfig::kFontOversample };
        SDL_RenderCopy(renderer, fgct.texture, NULL, &fgRect);
    }
}

void RenderSystem::DrawTextCentered(TTF_Font* font, const std::string& text, SDL_Color color, SDL_Rect bounds) {
    if (!font || text.empty()) return;
    int w = 0, h = 0;
    TTF_SizeUTF8(font, text.c_str(), &w, &h);
    int x = bounds.x + (bounds.w - w / EngineConfig::kFontOversample) / 2;
    int y = bounds.y + (bounds.h - h / EngineConfig::kFontOversample) / 2;
    DrawText(font, text, color, x, y);
}

void RenderSystem::DrawTextWithOutlineCentered(TTF_Font* font, const std::string& text, SDL_Color textColor, SDL_Color outlineColor, int outlineSize, SDL_Rect bounds, Uint8 alpha, bool shadow) {
    if (!font || text.empty()) return;
    int w = 0, h = 0;
    TTF_SizeUTF8(font, text.c_str(), &w, &h);
    int x = bounds.x + (bounds.w - w / EngineConfig::kFontOversample) / 2 - outlineSize;
    int y = bounds.y + (bounds.h - h / EngineConfig::kFontOversample) / 2 - outlineSize;
    DrawTextWithOutline(font, text, textColor, outlineColor, outlineSize, x, y, 0, alpha, shadow);
}

} // namespace PrismatiX::Systems
