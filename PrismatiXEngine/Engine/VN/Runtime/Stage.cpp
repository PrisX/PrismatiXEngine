#include "Engine/VN/Runtime/Stage.h"

#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

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
    m_prevBgPath = m_bgPath;
    m_bgPath = path;
    m_bgFade = (transition && !m_prevBgPath.empty()) ? 0.0f : 1.0f;
}

void Stage::SetCharacter(const std::string& name, const std::string& imagePath, int slot,
                         bool transition) {
    Actor& actor = m_actors[name];
    const bool isNew = actor.imagePath.empty();
    actor.imagePath = imagePath;
    actor.slot = slot;
    actor.exiting = false;
    actor.targetAlpha = 255.0f;
    actor.targetX = SlotCenterX(slot);
    if (isNew) {
        actor.alpha = transition ? 0.0f : 255.0f;
        actor.x = transition ? actor.targetX + kSlideOffset : actor.targetX;
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

void Stage::ClearAll() {
    m_actors.clear();
    m_bgPath.clear();
    m_prevBgPath.clear();
    m_bgFade = 1.0f;
}

void Stage::Update(float dt) {
    if (m_bgFade < 1.0f) {
        m_bgFade = std::min(1.0f, m_bgFade + kBgFadeSpeed * dt);
        if (m_bgFade >= 1.0f) {
            m_prevBgPath.clear();
        }
    }

    for (auto it = m_actors.begin(); it != m_actors.end();) {
        Actor& a = it->second;
        a.alpha = Approach(a.alpha, a.targetAlpha, kAlphaSpeed, dt);
        a.x = Approach(a.x, a.targetX, kSlideSpeed, dt);
        if (a.exiting && a.alpha <= 1.0f) {
            it = m_actors.erase(it);
        } else {
            ++it;
        }
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

    std::vector<const Actor*> ordered;
    ordered.reserve(m_actors.size());
    for (const auto& [name, actor] : m_actors) {
        ordered.push_back(&actor);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const Actor* a, const Actor* b) { return a->slot < b->slot; });

    int logicalW = 0, logicalH = 0;
    m_renderer.GetLogicalSize(logicalW, logicalH);
    for (const Actor* a : ordered) {
        SDL_Texture* tex = m_assets.Texture(a->imagePath);
        if (!tex) {
            continue;
        }
        int tw = 0, th = 0;
        graphics::AssetCache::TextureSize(tex, tw, th);
        if (tw <= 0 || th <= 0) {
            continue;
        }
        const float drawX = a->x - tw * 0.5f;
        const float drawY = static_cast<float>(logicalH - th);
        m_renderer.DrawImage(a->imagePath,
                             Rect{ drawX, drawY, static_cast<float>(tw), static_cast<float>(th) },
                             static_cast<std::uint8_t>(std::clamp(a->alpha, 0.0f, 255.0f)));
    }
}

std::vector<Stage::SavedActor> Stage::Snapshot() const {
    std::vector<SavedActor> out;
    out.reserve(m_actors.size());
    for (const auto& [name, actor] : m_actors) {
        if (!actor.exiting) {
            out.push_back(SavedActor{ name, actor.imagePath, actor.slot });
        }
    }
    return out;
}

void Stage::Restore(const std::vector<SavedActor>& actors) {
    m_actors.clear();
    for (const SavedActor& s : actors) {
        SetCharacter(s.name, s.imagePath, s.slot, /*transition=*/false);
    }
}

}
