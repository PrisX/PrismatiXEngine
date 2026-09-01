#include "Engine/VN/Runtime/Stage.h"

#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Support/Easing.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

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
    const std::string oldBackgroundPath = m_bgPath;
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

    rs.texture = graphics::TextureResource::CreateStreaming(
        graphics::TextureBackend::SdlRenderer, m_renderer.Handle(),
        graphics::StreamingTextureFormat::Rgba32, w, h);
    if (!rs.texture) {
        rs = RuleState{};
        SetBackground(newBgPath, true);
        return;
    }
    (void)rs.texture.SetAlphaBlend();
    (void)rs.texture.SetLinearSampling();
    rs.active = true;
    rs.oldBackgroundPath = oldBackgroundPath;
    rs.newBackgroundPath = newBgPath;
    rs.rulePath = rulePath;
    rs.progress = 0.0f;
    rs.duration = std::max(0.05f, static_cast<float>(durationMs) / 1000.0f);
    rs.vague = std::max(1, vague);

    // The new background draws underneath; the old one dissolves out on top.
    m_prevBgPath.clear();
    m_bgFade = 1.0f;
    m_bgPath = newBgPath;
}

void Stage::EndRuleTransition() {
    m_ruleState = RuleState{};
}

void Stage::SetLayer(const std::string& name, const std::string& imagePath, float x, float y,
                     float scale, std::uint8_t alpha, int z) {
    SetLayerTransform(name, imagePath, x, y, scale, scale, 0.0f, alpha, z);
}

void Stage::SetLayerTransform(const std::string& name, const std::string& imagePath,
                              const float x, const float y, const float scaleX,
                              const float scaleY, const float rotation,
                              const std::uint8_t alpha, const int z) {
    if (name.empty()) {
        return;
    }
    Layer& layer = m_layers[name];
    layer.imagePath = imagePath;
    layer.x = x;
    layer.y = y;
    layer.scale = scaleX > 0.0f ? scaleX : 1.0f;
    layer.scaleY = scaleY > 0.0f ? scaleY : 1.0f;
    layer.rotation = std::isfinite(rotation) ? rotation : 0.0f;
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

bool Stage::ApplyAnimationProperty(const std::string& target, const std::string& property,
                                   const Variant& value) {
    const auto number = [&value]() -> std::optional<float> {
        if (const auto* integer = value.TryGet<std::int64_t>()) return static_cast<float>(*integer);
        if (const auto* real = value.TryGet<double>()) return static_cast<float>(*real);
        return std::nullopt;
    }();
    if (!number) return false;
    if (target == "$camera" || target == "camera") {
        if (property == "x") m_cameraX = *number;
        else if (property == "y") m_cameraY = *number;
        else if(property=="zoom")m_cameraZoom=std::max(.01f,*number);
        else if(property=="shake"){m_screenEffects[property]=*number;if(*number>0)Shake(120,*number*18.0f);}
        else if(property=="pan")m_cameraX=*number*120.0f;
        else if(property=="flash"||property=="fade")m_screenEffects[property]=std::clamp(*number,0.0f,1.0f);
        else if(property=="blur"||property=="vignette"||property=="color-grade"){
            if(!m_renderer.SupportsStagePostEffects())return false;
            m_screenEffects[property]=std::clamp(*number,0.0f,1.0f);
        }
        else if(property=="rule-dissolve"){
            if(!m_ruleState.active)return false;
            m_ruleState.progress=std::clamp(*number,0.0f,.999999f);
        }
        else if (m_renderer.HasCustomEffect(property)) {
            if (m_customEffect != property) {
                const auto defaults = m_renderer.CustomEffectDefaults(property);
                if (!defaults) return false;
                m_customEffect = property;
                m_customEffectParameters = *defaults;
                ++m_customEffectSequence;
                m_customEffectSeed = m_customEffectSeed * 1664525u +
                                     1013904223u + m_customEffectSequence;
            }
            m_customEffectProgress = std::clamp(*number, 0.0f, 1.0f);
        }
        else return false;
        return true;
    }
    if (auto actor = m_actors.find(target); actor != m_actors.end()) {
        if (property == "x") actor->second.offsetX = *number;
        else if (property == "y") actor->second.offsetY = *number;
        else if (property == "scale") actor->second.scale = *number;
        else if (property == "alpha") actor->second.alpha = std::clamp(*number, 0.0f, 255.0f);
        else if (property == "fade" || property == "expression-crossfade")
            actor->second.effectAlpha = std::clamp(*number, 0.0f, 1.0f);
        else if (property == "enter-left")
            actor->second.effectOffsetX = (*number - 1.0f) * 500.0f;
        else if (property == "enter-right")
            actor->second.effectOffsetX = (1.0f - *number) * 500.0f;
        else if (property == "exit-left")
            actor->second.effectOffsetX = -*number * 500.0f;
        else if (property == "exit-right")
            actor->second.effectOffsetX = *number * 500.0f;
        else if (property == "hop" || property == "shake")
            actor->second.effectOffsetY =
                std::sin(*number * 18.8495559f) * (property == "hop" ? -24.0f : 7.0f);
        else if (property == "breathing")
            actor->second.effectScale = 1.0f + std::sin(*number * 6.2831853f) * .018f;
        else if (property == "blink") {
            const float phase = std::sin(std::clamp(*number, 0.0f, 1.0f) * 3.14159265f);
            actor->second.effectAlpha = 1.0f - std::pow(std::abs(phase), 8.0f) * .92f;
        } else if (property == "lip-sync") {
            const float mouth = std::abs(std::sin(*number * 18.8495559f));
            actor->second.effectScale = 1.0f + mouth * .018f;
            actor->second.effectOffsetY = -mouth * 2.5f;
        }
        else return false;
        return true;
    }
    if (auto layer = m_layers.find(target); layer != m_layers.end()) {
        if (property == "x") layer->second.x = *number;
        else if (property == "y") layer->second.y = *number;
        else if (property == "scale" || property == "scaleX") layer->second.scale = *number;
        else if (property == "scaleY") layer->second.scaleY = *number;
        else if (property == "rotation") layer->second.rotation = *number;
        else if (property == "alpha") layer->second.alpha = static_cast<std::uint8_t>(
            std::clamp(*number, 0.0f, 255.0f));
        else if (property == "opacity") layer->second.alpha = static_cast<std::uint8_t>(
            std::clamp(*number, 0.0f, 1.0f) * 255.0f);
        else return false;
        return true;
    }
    return false;
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
    m_cameraX = m_cameraY = 0.0f;
    m_cameraZoom = 1.0f;
    m_screenEffects.clear();
    m_customEffect.clear();
    m_customEffectProgress = 0.0f;
    m_customEffectSeed = 0x6d2b79f5u;
    m_customEffectSequence = 0;
    m_customEffectParameters = {};
    m_renderer.ResetCamera();
}

void Stage::Shake(int durationMs, float amplitude) {
    m_shakeDuration = m_shakeRemaining = std::max(0.0f, durationMs / 1000.0f);
    m_shakeAmplitude = amplitude;
    m_shakePhase = 0.0f;
}

void Stage::Update(float dt) {
    float shakeX = 0.0f;
    float shakeY = 0.0f;
    if (m_shakeRemaining > 0.0f) {
        m_shakeRemaining = std::max(0.0f, m_shakeRemaining - dt);
        m_shakePhase += dt * 55.0f;
        if (m_shakeRemaining <= 0.0f) {
        } else {
            const float falloff = m_shakeDuration > 0.0f ? m_shakeRemaining / m_shakeDuration : 0.0f;
            const float amp = m_shakeAmplitude * falloff;
            shakeX = std::sin(m_shakePhase) * amp;
            shakeY = std::cos(m_shakePhase * 1.3f) * amp * 0.6f;
        }
    }
    m_renderer.SetCameraOffset(static_cast<int>(std::lround(m_cameraX + shakeX)),
                               static_cast<int>(std::lround(m_cameraY + shakeY)));

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
    (void)rs.texture.Update(rs.oldPixels.data(),
                            static_cast<std::size_t>(rs.w) * 4u);
    m_renderer.DrawTexture(rs.texture.Handle(),
                           Rect{0.0f, 0.0f, static_cast<float>(rs.w),
                                static_cast<float>(rs.h)});
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
        const graphics::TextureHandle texture =
            m_assets.Texture(layer->imagePath);
        if (!texture) {
            continue;
        }
        int tw = 0, th = 0;
        graphics::AssetCache::TextureSize(texture, tw, th);
        if (tw <= 0 || th <= 0) {
            continue;
        }
        const Rect bounds{ layer->x, layer->y, tw * layer->scale,
                           th * layer->scaleY };
        m_renderer.PushTransform(
            {bounds.x + bounds.w * 0.5f, bounds.y + bounds.h * 0.5f},
            {1.0f, 1.0f}, layer->rotation);
        m_renderer.DrawImage(layer->imagePath, bounds, layer->alpha);
        m_renderer.PopTransform();
    }
}

void Stage::Render() {
    const bool compositing = m_renderer.BeginStageLayer();
    int logicalW = 0, logicalH = 0;
    m_renderer.GetLogicalSize(logicalW, logicalH);
    m_renderer.PushTransform(
        {static_cast<float>(logicalW) * 0.5f,
         static_cast<float>(logicalH) * 0.5f},
        {m_cameraZoom, m_cameraZoom});
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

    const auto drawSprite = [&](const std::string& path, const Actor& actor, float alpha) {
        const graphics::TextureHandle texture = m_assets.Texture(path);
        if (!texture) {
            return;
        }
        int tw = 0, th = 0;
        graphics::AssetCache::TextureSize(texture, tw, th);
        if (tw <= 0 || th <= 0) {
            return;
        }
        const float effectScale = std::max(.01f, actor.effectScale);
        const float w = tw * actor.scale * effectScale;
        const float h = th * actor.scale * effectScale;
        const float drawX = actor.x - w * 0.5f + actor.offsetX + actor.effectOffsetX;
        const float drawY = static_cast<float>(logicalH) - h + actor.offsetY +
                            actor.effectOffsetY;
        m_renderer.DrawImage(path, Rect{ drawX, drawY, w, h },
                             static_cast<std::uint8_t>(std::clamp(
                                 alpha * actor.effectAlpha, 0.0f, 255.0f)));
    };
    for (const Actor* a : ordered) {
        if (!a->prevImagePath.empty()) {
            drawSprite(a->prevImagePath, *a, a->prevAlpha);
        }
        drawSprite(a->imagePath, *a, a->alpha);
    }
    RenderLayers(/*front=*/true);
    m_renderer.PopTransform();
    // Camera and shake belong to the Stage layer. Dialogue/UI is rendered by
    // the host after this call and must remain stable.
    m_renderer.ResetCamera();
    RenderScreenEffects();
    if (compositing) {
        const auto effect = [this](const char* name) {
            const auto found = m_screenEffects.find(name);
            return found == m_screenEffects.end() ? 0.0f : found->second;
        };
        graphics::StagePostEffects effects{
            .blur = effect("blur"),
            .vignette = effect("vignette"),
            .colorGrade = effect("color-grade")};
        effects.randomSeed = static_cast<float>(m_customEffectSeed);
        effects.customEffect = m_customEffect;
        effects.customProgress = m_customEffectProgress;
        effects.customParameters = m_customEffectParameters;
        m_renderer.EndStageLayer(effects);
    }
}

void Stage::RenderScreenEffects(){int width=0,height=0;m_renderer.GetLogicalSize(width,height);const auto effect=[this](const char* name){const auto found=m_screenEffects.find(name);return found==m_screenEffects.end()?0.0f:found->second;};const float fade=effect("fade"),flash=effect("flash");if(fade>0)m_renderer.DrawRect({0,0,static_cast<float>(width),static_cast<float>(height)},{0,0,0,static_cast<std::uint8_t>(fade*255)});if(flash>0)m_renderer.DrawRect({0,0,static_cast<float>(width),static_cast<float>(height)},{255,255,255,static_cast<std::uint8_t>(flash*230)});}

std::vector<Stage::SavedActor> Stage::Snapshot() const {
    std::vector<SavedActor> out;
    out.reserve(m_actors.size());
    for (const auto& [name, actor] : m_actors) {
        if (!actor.exiting) {
            out.push_back(SavedActor{ name, actor.imagePath, actor.slot, actor.offsetX,
                                      actor.offsetY, actor.scale, actor.prevImagePath,
                                      actor.alpha, actor.targetAlpha, actor.prevAlpha,
                                      actor.x, actor.targetX, actor.exiting,
                                      actor.effectOffsetX, actor.effectOffsetY,
                                      actor.effectScale, actor.effectAlpha });
        }
    }
    return out;
}

void Stage::Restore(const std::vector<SavedActor>& actors) {
    m_actors.clear();
    for (const SavedActor& s : actors) {
        SetCharacter(s.name, s.imagePath, s.slot, /*transition=*/false, s.offsetX, s.offsetY,
                     s.scale);
        auto restored = m_actors.find(s.name);
        if (restored == m_actors.end()) continue;
        restored->second.effectOffsetX = s.effectOffsetX;
        restored->second.effectOffsetY = s.effectOffsetY;
        restored->second.effectScale = s.effectScale;
        restored->second.effectAlpha = s.effectAlpha;
    }
}

std::vector<Stage::SavedLayer> Stage::SnapshotLayers() const {
    std::vector<SavedLayer> out;
    out.reserve(m_layers.size());
    for (const auto& [name, layer] : m_layers) {
        out.push_back(SavedLayer{ name, layer.imagePath, layer.x, layer.y, layer.scale,
                                  layer.alpha, layer.z, layer.scaleY, layer.rotation });
    }
    return out;
}

void Stage::RestoreLayers(const std::vector<SavedLayer>& layers) {
    m_layers.clear();
    for (const SavedLayer& s : layers) {
        SetLayerTransform(s.name, s.imagePath, s.x, s.y, s.scale, s.scaleY,
                          s.rotation,
                          static_cast<std::uint8_t>(std::clamp(s.alpha, 0, 255)),
                          s.z);
    }
}

Stage::RuntimeState Stage::CaptureState() const {
    RuntimeState state;
    state.background = m_bgPath;
    state.previousBackground = m_prevBgPath;
    state.backgroundFade = m_bgFade;
    state.ruleActive = m_ruleState.active;
    state.ruleOldBackground = m_ruleState.oldBackgroundPath;
    state.ruleNewBackground = m_ruleState.newBackgroundPath;
    state.ruleMask = m_ruleState.rulePath;
    state.ruleProgress = m_ruleState.progress;
    state.ruleDuration = m_ruleState.duration;
    state.ruleVague = m_ruleState.vague;
    state.cameraX = m_cameraX;
    state.cameraY = m_cameraY;
    state.cameraZoom = m_cameraZoom;
    state.shakeRemaining = m_shakeRemaining;
    state.shakeDuration = m_shakeDuration;
    state.shakeAmplitude = m_shakeAmplitude;
    state.shakePhase = m_shakePhase;
    state.screenEffects = m_screenEffects;
    state.customEffect = m_customEffect;
    state.customEffectProgress = m_customEffectProgress;
    state.customEffectSeed = m_customEffectSeed;
    state.customEffectSequence = m_customEffectSequence;
    state.customEffectParameters = m_customEffectParameters;
    state.actors = Snapshot();
    // Exiting actors still affect the next frame and therefore must not be
    // discarded by the creator-oriented Snapshot() helper.
    for (const auto& [name, actor] : m_actors) {
        if (!actor.exiting) continue;
        state.actors.push_back(SavedActor{name, actor.imagePath, actor.slot, actor.offsetX,
                                           actor.offsetY, actor.scale, actor.prevImagePath,
                                           actor.alpha, actor.targetAlpha, actor.prevAlpha,
                                           actor.x, actor.targetX, true,
                                           actor.effectOffsetX, actor.effectOffsetY,
                                           actor.effectScale, actor.effectAlpha});
    }
    state.layers = SnapshotLayers();
    state.tweens.reserve(m_tweens.size());
    for (const Tween& tween : m_tweens) {
        state.tweens.push_back(SavedTween{tween.isLayer, tween.name, tween.spec,
                                          tween.fromX, tween.fromY, tween.fromScale,
                                          tween.fromAlpha, tween.elapsed, tween.duration});
    }
    return state;
}

Status Stage::RestoreState(const RuntimeState& state) {
    const auto finite = [](const float value) { return std::isfinite(value); };
    if (!finite(state.backgroundFade) || !finite(state.ruleProgress) ||
        !finite(state.ruleDuration) ||
        (state.ruleActive &&
         (state.ruleOldBackground.empty() || state.ruleNewBackground.empty() ||
          state.ruleMask.empty() || state.ruleProgress < 0.0f ||
          state.ruleProgress >= 1.0f || state.ruleDuration <= 0.0f ||
          state.ruleVague < 1)) ||
        !finite(state.cameraX) ||
        !finite(state.cameraY) || !finite(state.cameraZoom) || state.cameraZoom <= 0.0f ||
        !finite(state.shakeRemaining) || !finite(state.shakeDuration) ||
        !finite(state.shakeAmplitude) || !finite(state.shakePhase) ||
        !finite(state.customEffectProgress) ||
        state.customEffectProgress < 0.0f || state.customEffectProgress > 1.0f ||
        (!state.customEffect.empty() &&
         !m_renderer.HasCustomEffect(state.customEffect))) {
        return Status::Fail(diag::Diagnostic{.severity = diag::Severity::Error,
                                             .code = "PXSTAGE7510",
                                             .category = "Runtime.Stage",
                                             .message = "Saved stage state contains invalid numbers"});
    }
    ClearAll();
    m_bgPath = state.background;
    m_prevBgPath = state.previousBackground;
    m_bgFade = std::clamp(state.backgroundFade, 0.0f, 1.0f);
    m_cameraX = state.cameraX;
    m_cameraY = state.cameraY;
    m_cameraZoom = state.cameraZoom;
    m_shakeRemaining = std::max(0.0f, state.shakeRemaining);
    m_shakeDuration = std::max(0.0f, state.shakeDuration);
    m_shakeAmplitude = state.shakeAmplitude;
    m_shakePhase = state.shakePhase;
    m_screenEffects = state.screenEffects;
    m_customEffect = state.customEffect;
    m_customEffectProgress = state.customEffectProgress;
    m_customEffectSeed = state.customEffectSeed;
    m_customEffectSequence = state.customEffectSequence;
    m_customEffectParameters = state.customEffectParameters;
    for (const auto& slot : m_customEffectParameters)
        for (const float component : slot)
            if (!finite(component))
                return Status::Fail(diag::Diagnostic{
                    .severity = diag::Severity::Error,
                    .code = "PXSTAGE7510",
                    .category = "Runtime.Stage",
                    .message = "Saved custom effect parameters contain invalid numbers"});
    for (const SavedActor& saved : state.actors) {
        if (saved.name.empty() || !finite(saved.alpha) || !finite(saved.targetAlpha) ||
            !finite(saved.previousAlpha) || !finite(saved.x) || !finite(saved.targetX) ||
            !finite(saved.offsetX) || !finite(saved.offsetY) || !finite(saved.scale) ||
            saved.scale <= 0.0f || !finite(saved.effectOffsetX) ||
            !finite(saved.effectOffsetY) || !finite(saved.effectScale) ||
            saved.effectScale <= 0.0f || !finite(saved.effectAlpha) ||
            saved.effectAlpha < 0.0f || saved.effectAlpha > 1.0f) {
            return Status::Fail(diag::Diagnostic{.severity = diag::Severity::Error,
                                                 .code = "PXSTAGE7511",
                                                 .category = "Runtime.Stage",
                                                 .message = "Saved actor state is invalid",
                                                 .details = saved.name});
        }
        Actor actor;
        actor.imagePath = saved.imagePath;
        actor.prevImagePath = saved.previousImagePath;
        actor.slot = saved.slot;
        actor.alpha = saved.alpha;
        actor.targetAlpha = saved.targetAlpha;
        actor.prevAlpha = saved.previousAlpha;
        actor.x = saved.x;
        actor.targetX = saved.targetX;
        actor.offsetX = saved.offsetX;
        actor.offsetY = saved.offsetY;
        actor.scale = saved.scale;
        actor.effectOffsetX = saved.effectOffsetX;
        actor.effectOffsetY = saved.effectOffsetY;
        actor.effectScale = saved.effectScale;
        actor.effectAlpha = saved.effectAlpha;
        actor.exiting = saved.exiting;
        m_actors.emplace(saved.name, std::move(actor));
    }
    RestoreLayers(state.layers);
    m_tweens.clear();
    for (const SavedTween& saved : state.tweens) {
        if (saved.target.empty() || !finite(saved.elapsed) || !finite(saved.duration) ||
            saved.duration <= 0.0f || saved.elapsed < 0.0f || saved.elapsed > saved.duration) {
            return Status::Fail(diag::Diagnostic{.severity = diag::Severity::Error,
                                                 .code = "PXSTAGE7512",
                                                 .category = "Runtime.Stage",
                                                 .message = "Saved stage animation is invalid",
                                                 .details = saved.target});
        }
        m_tweens.push_back(Tween{saved.layer, saved.target, saved.spec, saved.fromX,
                                  saved.fromY, saved.fromScale, saved.fromAlpha,
                                  saved.elapsed, saved.duration});
    }
    if (state.ruleActive) {
        m_bgPath = state.ruleOldBackground;
        SetBackgroundRule(state.ruleNewBackground, state.ruleMask,
                          std::max(1, static_cast<int>(std::lround(state.ruleDuration * 1000.0f))),
                          state.ruleVague);
        if (!m_ruleState.active) {
            return Status::Fail(diag::Diagnostic{.severity = diag::Severity::Error,
                                                 .code = "PXSTAGE7513",
                                                 .category = "Runtime.Stage",
                                                 .message = "Saved rule transition assets cannot be restored",
                                                 .details = state.ruleMask});
        }
        m_ruleState.progress = state.ruleProgress;
    }
    return Status::Ok();
}

}
