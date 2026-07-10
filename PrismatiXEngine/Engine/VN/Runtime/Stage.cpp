#include "Engine/VN/Runtime/Stage.h"

#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Support/Easing.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace px::vn {

namespace {
constexpr float kBgFadeSpeed = 2.5f;
constexpr float kAlphaSpeed = 6.0f;
constexpr float kSlideSpeed = 9.0f;
constexpr float kSlideOffset = 40.0f;

float Approach(float value, float target, float rate, float dt) {
    const float t = std::min(1.0f, rate * dt);
    return value + (target - value) * t;
}
}

Stage::Stage(graphics::Renderer2D& renderer, graphics::AssetCache& assets)
    : m_renderer(renderer), m_assets(assets) {}

Stage::~Stage() {
    EndRuleTransition();
}

float Stage::SlotCenterX(int slot) const {
    int w = 0, h = 0;
    m_renderer.GetLogicalSize(w, h);
    switch (slot) {
        case 1: return w * 0.25f;
        case 3: return w * 0.75f;
        default: return w * 0.5f;
    }
}

void Stage::SetBackground(const std::string& path, bool transition) {
    if (path == m_bgPath) {
        return;
    }
    EndRuleTransition();
    m_prevBgPath = m_bgPath;
    m_bgPath = path;
    m_bgFade = (transition && !m_prevBgPath.empty()) ? 0.0f : 1.0f;
}

void Stage::SetBackgroundRule(const std::string& newBgPath, const std::string& rulePath,
                              int durationMs, int vague) {
    EndRuleTransition();
    if (newBgPath == m_bgPath) {
        return;
    }
    int w = 0, h = 0;
    m_renderer.GetLogicalSize(w, h);
    SDL_Surface* oldSurf = m_bgPath.empty() ? nullptr : m_assets.LoadSurface(m_bgPath);
    SDL_Surface* ruleSurf = m_assets.LoadSurface(rulePath);
    if (!oldSurf || !ruleSurf || w <= 0 || h <= 0) {
        if (oldSurf) SDL_DestroySurface(oldSurf);
        if (ruleSurf) SDL_DestroySurface(ruleSurf);
        SetBackground(newBgPath, true);
        return;
    }
    SDL_Surface* oldScaled = SDL_ScaleSurface(oldSurf, w, h, SDL_SCALEMODE_LINEAR);
    SDL_Surface* ruleScaled = SDL_ScaleSurface(ruleSurf, w, h, SDL_SCALEMODE_LINEAR);
    SDL_DestroySurface(oldSurf);
    SDL_DestroySurface(ruleSurf);
    if (!oldScaled || !ruleScaled) {
        if (oldScaled) SDL_DestroySurface(oldScaled);
        if (ruleScaled) SDL_DestroySurface(ruleScaled);
        SetBackground(newBgPath, true);
        return;
    }

    RuleState& rs = m_ruleState;
    rs.w = w;
    rs.h = h;
    rs.oldPixels.resize(static_cast<std::size_t>(w) * h * 4);
    const auto* oldPx = static_cast<const std::uint8_t*>(oldScaled->pixels);
    for (int row = 0; row < h; ++row) {
        std::memcpy(rs.oldPixels.data() + static_cast<std::size_t>(row) * w * 4,
                    oldPx + static_cast<std::size_t>(row) * oldScaled->pitch,
                    static_cast<std::size_t>(w) * 4);
    }
    rs.rule.resize(static_cast<std::size_t>(w) * h);
    const auto* rulePx = static_cast<const std::uint8_t*>(ruleScaled->pixels);
    for (int row = 0; row < h; ++row) {
        const std::uint8_t* line = rulePx + static_cast<std::size_t>(row) * ruleScaled->pitch;
        for (int col = 0; col < w; ++col) {
            rs.rule[static_cast<std::size_t>(row) * w + col] = line[col * 4];  // R channel
        }
    }
    SDL_DestroySurface(oldScaled);
    SDL_DestroySurface(ruleScaled);

    rs.texture = SDL_CreateTexture(m_renderer.Handle(), SDL_PIXELFORMAT_RGBA32,
                                   SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!rs.texture) {
        rs = RuleState{};
        SetBackground(newBgPath, true);
        return;
    }
    SDL_SetTextureBlendMode(rs.texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(rs.texture, SDL_SCALEMODE_LINEAR);
    rs.active = true;
    rs.progress = 0.0f;
    rs.duration = std::max(0.05f, static_cast<float>(durationMs) / 1000.0f);
    rs.vague = std::max(1, vague);

    // The new background draws underneath; the old one dissolves out on top.
    m_prevBgPath.clear();
    m_bgFade = 1.0f;
    m_bgPath = newBgPath;
}

void Stage::EndRuleTransition() {
    if (m_ruleState.texture) {
        SDL_DestroyTexture(m_ruleState.texture);
    }
    m_ruleState = RuleState{};
}

void Stage::SetLayer(const std::string& name, const std::string& imagePath, float x, float y,
                     float scale, std::uint8_t alpha, int z) {
    if (name.empty()) {
        return;
    }
    Layer& layer = m_layers[name];
    layer.imagePath = imagePath;
    layer.x = x;
    layer.y = y;
    layer.scale = scale > 0.0f ? scale : 1.0f;
    layer.alpha = alpha;
    layer.z = z;
}

void Stage::ClearLayer(const std::string& name) {
    m_layers.erase(name);
}

bool Stage::Animate(const std::string& target, const TweenSpec& spec) {
    if (target.empty()) {
        return false;
    }
    Tween tween;
    tween.name = target;
    tween.spec = spec;
    tween.duration = std::max(0.001f, static_cast<float>(spec.durationMs) / 1000.0f);

    if (auto it = m_actors.find(target); it != m_actors.end()) {
        Actor& actor = it->second;
        tween.isLayer = false;
        tween.fromX = actor.offsetX;
        tween.fromY = actor.offsetY;
        tween.fromScale = actor.scale;
        tween.fromAlpha = actor.alpha;
        if (spec.hasAlpha) {
            // Keep the per-frame Approach in agreement so nothing snaps back
            // once the tween finishes.
            actor.targetAlpha = spec.alpha;
        }
    } else if (auto lit = m_layers.find(target); lit != m_layers.end()) {
        const Layer& layer = lit->second;
        tween.isLayer = true;
        tween.fromX = layer.x;
        tween.fromY = layer.y;
        tween.fromScale = layer.scale;
        tween.fromAlpha = static_cast<float>(layer.alpha);
    } else {
        return false;
    }

    // One tween per target: a new animation replaces the running one.
    m_tweens.erase(std::remove_if(m_tweens.begin(), m_tweens.end(),
                                  [&](const Tween& t) { return t.name == target; }),
                   m_tweens.end());
    m_tweens.push_back(std::move(tween));
    return true;
}

void Stage::UpdateTweens(float dt) {
    for (auto it = m_tweens.begin(); it != m_tweens.end();) {
        Tween& tween = *it;
        tween.elapsed += dt;
        const float raw = std::clamp(tween.elapsed / tween.duration, 0.0f, 1.0f);
        const float t = support::Ease(tween.spec.ease, raw);
        const TweenSpec& spec = tween.spec;

        bool alive = true;
        if (!tween.isLayer) {
            auto ait = m_actors.find(tween.name);
            if (ait == m_actors.end()) {
                alive = false;  // actor was cleared mid-tween
            } else {
                Actor& actor = ait->second;
                if (spec.hasX) actor.offsetX = support::Lerp(tween.fromX, spec.x, t);
                if (spec.hasY) actor.offsetY = support::Lerp(tween.fromY, spec.y, t);
                if (spec.hasScale) actor.scale = support::Lerp(tween.fromScale, spec.scale, t);
                if (spec.hasAlpha) actor.alpha = support::Lerp(tween.fromAlpha, spec.alpha, t);
            }
        } else {
            auto lit = m_layers.find(tween.name);
            if (lit == m_layers.end()) {
                alive = false;
            } else {
                Layer& layer = lit->second;
                if (spec.hasX) layer.x = support::Lerp(tween.fromX, spec.x, t);
                if (spec.hasY) layer.y = support::Lerp(tween.fromY, spec.y, t);
                if (spec.hasScale) layer.scale = support::Lerp(tween.fromScale, spec.scale, t);
                if (spec.hasAlpha) {
                    layer.alpha = static_cast<std::uint8_t>(
                        std::clamp(support::Lerp(tween.fromAlpha, spec.alpha, t), 0.0f, 255.0f));
                }
            }
        }

        if (!alive || raw >= 1.0f) {
            it = m_tweens.erase(it);
        } else {
            ++it;
        }
    }
}

void Stage::SetCharacter(const std::string& name, const std::string& imagePath, int slot,
                         bool transition, float offsetX, float offsetY, float scale) {
    Actor& actor = m_actors[name];
    const bool isNew = actor.imagePath.empty();
    const bool imageChanged = !isNew && actor.imagePath != imagePath;
    if (imageChanged && transition) {
        // Expression change: crossfade the old sprite out under the new one.
        actor.prevImagePath = actor.imagePath;
        actor.prevAlpha = actor.alpha;
        actor.alpha = 0.0f;
    }
    actor.imagePath = imagePath;
    actor.slot = slot;
    actor.offsetX = offsetX;
    actor.offsetY = offsetY;
    actor.scale = scale > 0.0f ? scale : 1.0f;
    actor.exiting = false;
    actor.targetAlpha = 255.0f;
    actor.targetX = SlotCenterX(slot);
    if (isNew) {
        actor.alpha = transition ? 0.0f : 255.0f;
        actor.x = transition ? actor.targetX + kSlideOffset : actor.targetX;
    }
    if (!transition) {
        actor.prevImagePath.clear();
        actor.prevAlpha = 0.0f;
    }
}

void Stage::ClearCharacter(const std::string& name, bool transition) {
    auto it = m_actors.find(name);
    if (it == m_actors.end()) {
        return;
    }
    if (!transition) {
        m_actors.erase(it);
        return;
    }
    it->second.exiting = true;
    it->second.targetAlpha = 0.0f;
}

void Stage::MoveCharacter(const std::string& name, int slot) {
    auto it = m_actors.find(name);
    if (it == m_actors.end()) {
        return;
    }
    it->second.slot = slot;
    it->second.targetX = SlotCenterX(slot);
}

void Stage::ClearAll() {
    m_actors.clear();
    m_layers.clear();
    m_tweens.clear();
    EndRuleTransition();
    m_bgPath.clear();
    m_prevBgPath.clear();
    m_bgFade = 1.0f;
    m_shakeRemaining = 0.0f;
    m_renderer.ResetCamera();
}

void Stage::Shake(int durationMs, float amplitude) {
    m_shakeDuration = m_shakeRemaining = std::max(0.0f, durationMs / 1000.0f);
    m_shakeAmplitude = amplitude;
    m_shakePhase = 0.0f;
}

void Stage::Update(float dt) {
    if (m_shakeRemaining > 0.0f) {
        m_shakeRemaining = std::max(0.0f, m_shakeRemaining - dt);
        m_shakePhase += dt * 55.0f;
        if (m_shakeRemaining <= 0.0f) {
            m_renderer.ResetCamera();
        } else {
            const float falloff = m_shakeDuration > 0.0f ? m_shakeRemaining / m_shakeDuration : 0.0f;
            const float amp = m_shakeAmplitude * falloff;
            m_renderer.SetCameraOffset(static_cast<int>(std::sin(m_shakePhase) * amp),
                                       static_cast<int>(std::cos(m_shakePhase * 1.3f) * amp * 0.6f));
        }
    }

    if (m_bgFade < 1.0f) {
        m_bgFade = std::min(1.0f, m_bgFade + kBgFadeSpeed * dt);
        if (m_bgFade >= 1.0f) {
            m_prevBgPath.clear();
        }
    }

    if (m_ruleState.active) {
        m_ruleState.progress += dt / m_ruleState.duration;
        if (m_ruleState.progress >= 1.0f) {
            EndRuleTransition();
        }
    }

    for (auto it = m_actors.begin(); it != m_actors.end();) {
        Actor& a = it->second;
        a.alpha = Approach(a.alpha, a.targetAlpha, kAlphaSpeed, dt);
        a.x = Approach(a.x, a.targetX, kSlideSpeed, dt);
        if (!a.prevImagePath.empty()) {
            a.prevAlpha = Approach(a.prevAlpha, 0.0f, kAlphaSpeed, dt);
            if (a.prevAlpha <= 1.0f) {
                a.prevImagePath.clear();
                a.prevAlpha = 0.0f;
            }
        }
        if (a.exiting && a.alpha <= 1.0f) {
            it = m_actors.erase(it);
        } else {
            ++it;
        }
    }

    // Tweens run last so an animated property wins over the slot Approach.
    UpdateTweens(dt);
}

void Stage::RenderRuleOverlay() {
    RuleState& rs = m_ruleState;
    if (!rs.active || !rs.texture) {
        return;
    }
    // Per-pixel alpha for the outgoing background: pixels whose rule value is
    // below the sweeping threshold fade out over a band `vague` wide.
    const float p = rs.progress * (255.0f + static_cast<float>(rs.vague));
    const float invVague = 255.0f / static_cast<float>(rs.vague);
    std::uint8_t* px = rs.oldPixels.data();
    const std::size_t count = rs.rule.size();
    for (std::size_t i = 0; i < count; ++i) {
        const float a = (static_cast<float>(rs.rule[i]) + static_cast<float>(rs.vague) - p) *
                        invVague;
        px[i * 4 + 3] = static_cast<std::uint8_t>(std::clamp(a, 0.0f, 255.0f));
    }
    SDL_UpdateTexture(rs.texture, nullptr, rs.oldPixels.data(), rs.w * 4);
    m_renderer.DrawTexture(rs.texture, Rect{ 0.0f, 0.0f, static_cast<float>(rs.w),
                                             static_cast<float>(rs.h) });
}

void Stage::RenderLayers(bool front) {
    if (m_layers.empty()) {
        return;
    }
    std::vector<const Layer*> ordered;
    for (const auto& [name, layer] : m_layers) {
        if (front ? layer.z >= 0 : layer.z < 0) {
            ordered.push_back(&layer);
        }
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const Layer* a, const Layer* b) { return a->z < b->z; });
    for (const Layer* layer : ordered) {
        SDL_Texture* tex = m_assets.Texture(layer->imagePath);
        if (!tex) {
            continue;
        }
        int tw = 0, th = 0;
        graphics::AssetCache::TextureSize(tex, tw, th);
        if (tw <= 0 || th <= 0) {
            continue;
        }
        m_renderer.DrawImage(layer->imagePath,
                             Rect{ layer->x, layer->y, tw * layer->scale, th * layer->scale },
                             layer->alpha);
    }
}

void Stage::Render() {
    if (!m_prevBgPath.empty() && m_bgFade < 1.0f) {
        m_renderer.DrawImageAuto(m_prevBgPath, graphics::DisplayMode::Fill, 255);
    }
    if (!m_bgPath.empty()) {
        const auto alpha = static_cast<std::uint8_t>(std::clamp(m_bgFade, 0.0f, 1.0f) * 255.0f);
        m_renderer.DrawImageAuto(m_bgPath, graphics::DisplayMode::Fill, alpha);
    }
    RenderRuleOverlay();
    RenderLayers(/*front=*/false);

    std::vector<const Actor*> ordered;
    ordered.reserve(m_actors.size());
    for (const auto& [name, actor] : m_actors) {
        ordered.push_back(&actor);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const Actor* a, const Actor* b) { return a->slot < b->slot; });

    int logicalW = 0, logicalH = 0;
    m_renderer.GetLogicalSize(logicalW, logicalH);
    const auto drawSprite = [&](const std::string& path, const Actor& actor, float alpha) {
        SDL_Texture* tex = m_assets.Texture(path);
        if (!tex) {
            return;
        }
        int tw = 0, th = 0;
        graphics::AssetCache::TextureSize(tex, tw, th);
        if (tw <= 0 || th <= 0) {
            return;
        }
        const float w = tw * actor.scale;
        const float h = th * actor.scale;
        const float drawX = actor.x - w * 0.5f + actor.offsetX;
        const float drawY = static_cast<float>(logicalH) - h + actor.offsetY;
        m_renderer.DrawImage(path, Rect{ drawX, drawY, w, h },
                             static_cast<std::uint8_t>(std::clamp(alpha, 0.0f, 255.0f)));
    };
    for (const Actor* a : ordered) {
        if (!a->prevImagePath.empty()) {
            drawSprite(a->prevImagePath, *a, a->prevAlpha);
        }
        drawSprite(a->imagePath, *a, a->alpha);
    }
    RenderLayers(/*front=*/true);
}

std::vector<Stage::SavedActor> Stage::Snapshot() const {
    std::vector<SavedActor> out;
    out.reserve(m_actors.size());
    for (const auto& [name, actor] : m_actors) {
        if (!actor.exiting) {
            out.push_back(SavedActor{ name, actor.imagePath, actor.slot, actor.offsetX,
                                      actor.offsetY, actor.scale });
        }
    }
    return out;
}

void Stage::Restore(const std::vector<SavedActor>& actors) {
    m_actors.clear();
    for (const SavedActor& s : actors) {
        SetCharacter(s.name, s.imagePath, s.slot, /*transition=*/false, s.offsetX, s.offsetY,
                     s.scale);
    }
}

std::vector<Stage::SavedLayer> Stage::SnapshotLayers() const {
    std::vector<SavedLayer> out;
    out.reserve(m_layers.size());
    for (const auto& [name, layer] : m_layers) {
        out.push_back(SavedLayer{ name, layer.imagePath, layer.x, layer.y, layer.scale,
                                  layer.alpha, layer.z });
    }
    return out;
}

void Stage::RestoreLayers(const std::vector<SavedLayer>& layers) {
    m_layers.clear();
    for (const SavedLayer& s : layers) {
        SetLayer(s.name, s.imagePath, s.x, s.y, s.scale,
                 static_cast<std::uint8_t>(std::clamp(s.alpha, 0, 255)), s.z);
    }
}

}
