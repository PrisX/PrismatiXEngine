#include "Engine/VN/Runtime/Stage.h"

#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Support/Easing.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <ranges>
#include <unordered_set>

namespace px::vn {

namespace {
constexpr float kBgFadeSpeed = 2.5f;
constexpr float kAlphaSpeed = 6.0f;
constexpr float kSlideSpeed = 9.0f;
constexpr float kSlideOffset = 40.0f;
constexpr int kNodeOrderLimit = 1'000'000;

float Approach(float value, float target, float rate, float dt) {
    const float t = std::min(1.0f, rate * dt);
    return value + (target - value) * t;
}
}

Stage::Stage(graphics::Renderer2D& renderer, graphics::AssetCache& assets)
    : m_renderer(renderer), m_assets(assets), m_particles(renderer) {}

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
    Node& node = EnsureNode(name, NodeKind::Image);
    node.transform = NodeTransform{layer.x, layer.y, layer.scale, layer.scaleY,
                                   layer.rotation,
                                   static_cast<float>(layer.alpha) / 255.0f};
    node.z = layer.z;
}

void Stage::ClearLayer(const std::string& name) {
    if (const auto found = m_nodes.find(name);
        found != m_nodes.end() && found->second.kind == NodeKind::Image) {
        RemoveNode(name);
    } else {
        m_layers.erase(name);
    }
}

Stage::Node& Stage::EnsureNode(const std::string& name, const NodeKind kind) {
    const auto [found, inserted] = m_nodes.try_emplace(name);
    if (inserted) found->second.order = m_nextNodeOrder++;
    found->second.kind = kind;
    return found->second;
}

bool Stage::WouldCreateNodeCycle(const std::string& name,
                                 const std::string& parent) const {
    std::string cursor = parent;
    for (std::size_t depth = 0; !cursor.empty() && depth <= m_nodes.size(); ++depth) {
        if (cursor == name) return true;
        const auto found = m_nodes.find(cursor);
        if (found == m_nodes.end()) return false;
        cursor = found->second.parent;
    }
    return !cursor.empty();
}

bool Stage::SetGroupNode(const std::string& name, const std::string& parent) {
    if (name.empty() || name.size() > 128) return false;
    const bool existed = m_nodes.contains(name);
    if (const auto found = m_nodes.find(name);
        found != m_nodes.end() && found->second.kind != NodeKind::Group)
        return false;
    EnsureNode(name, NodeKind::Group);
    if (!SetNodeParent(name, parent)) {
        if (!existed) m_nodes.erase(name);
        return false;
    }
    return true;
}

bool Stage::SetNodeParent(const std::string& name, const std::string& parent) {
    auto found = m_nodes.find(name);
    if (found == m_nodes.end() || name == parent) return false;
    if (!parent.empty()) {
        const auto target = m_nodes.find(parent);
        if (target == m_nodes.end() || target->second.kind != NodeKind::Group ||
            WouldCreateNodeCycle(name, parent))
            return false;
    }
    if (!found->second.parent.empty()) {
        if (auto old = m_nodes.find(found->second.parent); old != m_nodes.end()) {
            std::erase(old->second.children, name);
        }
    }
    found->second.parent = parent;
    if (!parent.empty()) {
        auto& children = m_nodes.at(parent).children;
        if (std::ranges::find(children, name) == children.end())
            children.push_back(name);
    }
    return true;
}

bool Stage::SetNodeTransform(const std::string& name,
                             const NodeTransform& transform) {
    auto found = m_nodes.find(name);
    if (found == m_nodes.end() || !std::isfinite(transform.x) ||
        !std::isfinite(transform.y) || !std::isfinite(transform.scaleX) ||
        !std::isfinite(transform.scaleY) || !std::isfinite(transform.rotation) ||
        !std::isfinite(transform.opacity) || transform.scaleX <= 0.0f ||
        transform.scaleY <= 0.0f || transform.opacity < 0.0f ||
        transform.opacity > 1.0f)
        return false;
    found->second.transform = transform;
    if (found->second.kind == NodeKind::Image) {
        if (auto layer = m_layers.find(name); layer != m_layers.end()) {
            layer->second.x = transform.x;
            layer->second.y = transform.y;
            layer->second.scale = transform.scaleX;
            layer->second.scaleY = transform.scaleY;
            layer->second.rotation = transform.rotation;
            layer->second.alpha = static_cast<std::uint8_t>(
                std::lround(transform.opacity * 255.0f));
        }
    }
    return true;
}

bool Stage::SetNodeOrder(const std::string& name, const int z,
                         const int order) {
    auto found = m_nodes.find(name);
    if (found == m_nodes.end() || z < -kNodeOrderLimit ||
        z > kNodeOrderLimit || order < -kNodeOrderLimit ||
        order > kNodeOrderLimit)
        return false;
    found->second.z = z;
    found->second.order = order;
    m_nextNodeOrder = std::max(m_nextNodeOrder, order + 1);
    if (found->second.kind == NodeKind::Image) {
        if (auto layer = m_layers.find(name); layer != m_layers.end())
            layer->second.z = z;
    }
    return true;
}

bool Stage::SetNodeVisibility(const std::string& name, const bool visible) {
    auto found = m_nodes.find(name);
    if (found == m_nodes.end()) return false;
    found->second.visible = visible;
    return true;
}

void Stage::RemoveNode(const std::string& name) {
    auto found = m_nodes.find(name);
    if (found == m_nodes.end()) return;
    const std::vector<std::string> children = found->second.children;
    for (const auto& child : children) RemoveNode(child);
    if (!found->second.parent.empty()) {
        if (auto parent = m_nodes.find(found->second.parent);
            parent != m_nodes.end())
            std::erase(parent->second.children, name);
    }
    if (found->second.kind == NodeKind::Image) m_layers.erase(name);
    if (found->second.kind == NodeKind::Character) m_actors.erase(name);
    std::erase_if(m_tweens,
                  [&name](const Tween& tween) { return tween.name == name; });
    m_nodes.erase(found);
}

std::vector<Stage::SavedNode> Stage::SnapshotNodes() const {
    std::vector<SavedNode> out;
    out.reserve(m_nodes.size());
    for (const auto& [name, node] : m_nodes) {
        out.push_back({name, node.kind, node.parent, node.children, node.transform,
                       node.z, node.order, node.visible});
    }
    std::ranges::sort(out, [](const SavedNode& left, const SavedNode& right) {
        if (left.order != right.order) return left.order < right.order;
        return left.name < right.name;
    });
    return out;
}

bool Stage::SetParticleEmitter(const std::string& name,
                               const ParticleEmitterSpec& spec) {
    return m_particles.Set(name, spec);
}

void Stage::ClearParticleEmitter(const std::string_view name) {
    m_particles.Clear(name);
}

std::vector<ParticleSample> Stage::SampleParticles(
    const std::string_view name, const int width, const int height) const {
    return m_particles.Samples(name, width, height);
}

Stage::WorldNodeTransform Stage::ResolveNodeTransform(
    const std::string& name) const {
    WorldNodeTransform world;
    std::vector<const Node*> chain;
    std::string cursor = name;
    while (!cursor.empty() && chain.size() <= m_nodes.size()) {
        const auto found = m_nodes.find(cursor);
        if (found == m_nodes.end()) break;
        chain.push_back(&found->second);
        cursor = found->second.parent;
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const Node& node = **it;
        const float radians = world.rotation * 0.0174532925f;
        const float localX = node.transform.x * world.scaleX;
        const float localY = node.transform.y * world.scaleY;
        world.x += std::cos(radians) * localX - std::sin(radians) * localY;
        world.y += std::sin(radians) * localX + std::cos(radians) * localY;
        world.scaleX *= node.transform.scaleX;
        world.scaleY *= node.transform.scaleY;
        world.rotation += node.transform.rotation;
        world.opacity *= node.transform.opacity;
        world.visible = world.visible && node.visible;
    }
    return world;
}

Stage::NodeSortKey Stage::ResolveNodeSortKey(const std::string& name) const {
    NodeSortKey key;
    std::string cursor = name;
    while (!cursor.empty() && key.path.size() <= m_nodes.size()) {
        const auto found = m_nodes.find(cursor);
        if (found == m_nodes.end()) break;
        key.z += found->second.z;
        key.path.emplace_back(found->second.order, cursor);
        cursor = found->second.parent;
    }
    std::ranges::reverse(key.path);
    return key;
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
        if (auto node = m_nodes.find(target); node != m_nodes.end()) {
            node->second.transform.x = layer->second.x;
            node->second.transform.y = layer->second.y;
            node->second.transform.scaleX = layer->second.scale;
            node->second.transform.scaleY = layer->second.scaleY;
            node->second.transform.rotation = layer->second.rotation;
            node->second.transform.opacity =
                static_cast<float>(layer->second.alpha) / 255.0f;
        }
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
                if (auto node = m_nodes.find(tween.name); node != m_nodes.end()) {
                    node->second.transform.x = layer.x;
                    node->second.transform.y = layer.y;
                    node->second.transform.scaleX = layer.scale;
                    node->second.transform.scaleY = layer.scaleY;
                    node->second.transform.opacity =
                        static_cast<float>(layer.alpha) / 255.0f;
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
    if (name.empty()) return;
    Actor& actor = m_actors[name];
    Node& node = EnsureNode(name, NodeKind::Character);
    node.z = slot;
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
        RemoveNode(name);
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
    if (auto node = m_nodes.find(name); node != m_nodes.end()) node->second.z = slot;
}

void Stage::ClearAll() {
    m_actors.clear();
    m_layers.clear();
    m_nodes.clear();
    m_nextNodeOrder = 1;
    m_particles.ClearAll();
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

bool Stage::SetCamera(const float x, const float y, const float zoom) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(zoom) ||
        zoom <= 0.0f)
        return false;
    m_cameraX = x;
    m_cameraY = y;
    m_cameraZoom = zoom;
    return true;
}

bool Stage::SetScreenEffect(const std::string_view effect, const float amount) {
    if (!std::isfinite(amount)) return false;
    if (effect != "flash" && effect != "fade" && effect != "blur" &&
        effect != "vignette" && effect != "color-grade")
        return false;
    if ((effect == "blur" || effect == "vignette" ||
         effect == "color-grade") &&
        !m_renderer.SupportsStagePostEffects())
        return false;
    m_screenEffects[std::string(effect)] = std::clamp(amount, 0.0f, 1.0f);
    return true;
}

void Stage::ClearScreenEffect(const std::string_view effect) {
    m_screenEffects.erase(std::string(effect));
}

bool Stage::SetCustomEffect(
    const std::string_view effect, const float progress,
    const std::array<std::array<float, 4>, 8>* parameters) {
    if (!m_renderer.HasCustomEffect(effect) || !std::isfinite(progress))
        return false;
    if (m_customEffect != effect) {
        const auto defaults = m_renderer.CustomEffectDefaults(effect);
        if (!defaults) return false;
        m_customEffect = std::string(effect);
        m_customEffectParameters = *defaults;
        ++m_customEffectSequence;
        m_customEffectSeed = m_customEffectSeed * 1664525u +
                             1013904223u + m_customEffectSequence;
    }
    if (parameters) {
        for (const auto& slot : *parameters)
            for (const float component : slot)
                if (!std::isfinite(component)) return false;
        m_customEffectParameters = *parameters;
    }
    m_customEffectProgress = std::clamp(progress, 0.0f, 1.0f);
    return true;
}

bool Stage::SetCustomEffect(
    const std::string_view effect, const float progress,
    const graphics::CustomEffectNamedParameters& parameters) {
    const auto resolved =
        m_renderer.ResolveCustomEffectParameters(effect, parameters);
    return resolved && SetCustomEffect(effect, progress, &*resolved);
}

void Stage::ClearCustomEffect() {
    m_customEffect.clear();
    m_customEffectProgress = 0.0f;
    m_customEffectParameters = {};
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

    std::vector<std::string> expiredActors;
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
            expiredActors.push_back(it->first);
            it = m_actors.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& name : expiredActors) RemoveNode(name);

    // Tweens run last so an animated property wins over the slot Approach.
    UpdateTweens(dt);
    m_particles.Update(dt);
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
    struct Entry {
        std::string_view name;
        const Layer* layer = nullptr;
        NodeSortKey key;
    };
    std::vector<Entry> ordered;
    for (const auto& [name, layer] : m_layers) {
        const auto node = m_nodes.find(name);
        NodeSortKey key = node == m_nodes.end()
                              ? NodeSortKey{layer.z, {{0, name}}}
                              : ResolveNodeSortKey(name);
        if (front ? key.z >= 0 : key.z < 0)
            ordered.push_back({name, &layer, std::move(key)});
    }
    std::ranges::sort(ordered, [](const Entry& left, const Entry& right) {
        if (left.key.z != right.key.z) return left.key.z < right.key.z;
        if (left.key.path != right.key.path)
            return left.key.path < right.key.path;
        return left.name < right.name;
    });
    for (const Entry& entry : ordered) {
        const Layer* layer = entry.layer;
        const auto world = ResolveNodeTransform(std::string(entry.name));
        if (!world.visible || world.opacity <= 0.0f) continue;
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
        const Rect bounds{world.x, world.y, tw * world.scaleX,
                          th * world.scaleY};
        m_renderer.PushTransform(
            {bounds.x + bounds.w * 0.5f, bounds.y + bounds.h * 0.5f},
            {1.0f, 1.0f}, world.rotation);
        m_renderer.DrawImage(
            layer->imagePath, bounds,
            static_cast<std::uint8_t>(
                std::clamp(world.opacity, 0.0f, 1.0f) * 255.0f));
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
    m_particles.Render(/*front=*/false);

    struct ActorEntry {
        std::string_view name;
        const Actor* actor = nullptr;
        NodeSortKey key;
    };
    std::vector<ActorEntry> ordered;
    ordered.reserve(m_actors.size());
    for (const auto& [name, actor] : m_actors) {
        const auto node = m_nodes.find(name);
        NodeSortKey key = node == m_nodes.end()
                              ? NodeSortKey{actor.slot, {{0, name}}}
                              : ResolveNodeSortKey(name);
        ordered.push_back({name, &actor, std::move(key)});
    }
    std::ranges::sort(ordered, [](const ActorEntry& left,
                                  const ActorEntry& right) {
        if (left.key.z != right.key.z) return left.key.z < right.key.z;
        if (left.key.path != right.key.path)
            return left.key.path < right.key.path;
        return left.name < right.name;
    });

    const auto drawSprite = [&](const std::string& name, const std::string& path,
                                const Actor& actor, float alpha) {
        const auto world = ResolveNodeTransform(name);
        if (!world.visible || world.opacity <= 0.0f) return;
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
        const float w = tw * actor.scale * effectScale * world.scaleX;
        const float h = th * actor.scale * effectScale * world.scaleY;
        const float drawX = actor.x - w * 0.5f + actor.offsetX +
                            actor.effectOffsetX + world.x;
        const float drawY = static_cast<float>(logicalH) - h + actor.offsetY +
                            actor.effectOffsetY + world.y;
        m_renderer.PushTransform({drawX + w * 0.5f, drawY + h * 0.5f},
                                 {1.0f, 1.0f}, world.rotation);
        m_renderer.DrawImage(path, Rect{ drawX, drawY, w, h },
                             static_cast<std::uint8_t>(std::clamp(
                                 alpha * actor.effectAlpha * world.opacity,
                                 0.0f, 255.0f)));
        m_renderer.PopTransform();
    };
    for (const ActorEntry& entry : ordered) {
        const Actor* a = entry.actor;
        if (!a->prevImagePath.empty()) {
            drawSprite(std::string(entry.name), a->prevImagePath, *a,
                       a->prevAlpha);
        }
        drawSprite(std::string(entry.name), a->imagePath, *a, a->alpha);
    }
    RenderLayers(/*front=*/true);
    m_particles.Render(/*front=*/true);
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
    for (auto it = m_nodes.begin(); it != m_nodes.end();) {
        if (it->second.kind == NodeKind::Character) it = m_nodes.erase(it);
        else ++it;
    }
    for (auto& [_, node] : m_nodes) {
        std::erase_if(node.children, [this](const std::string& child) {
            return !m_nodes.contains(child);
        });
    }
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
    for (auto it = m_nodes.begin(); it != m_nodes.end();) {
        if (it->second.kind == NodeKind::Image) it = m_nodes.erase(it);
        else ++it;
    }
    for (auto& [_, node] : m_nodes) {
        std::erase_if(node.children, [this](const std::string& child) {
            return !m_nodes.contains(child);
        });
    }
    m_layers.clear();
    for (const SavedLayer& s : layers) {
        SetLayerTransform(s.name, s.imagePath, s.x, s.y, s.scale, s.scaleY,
                          s.rotation,
                          static_cast<std::uint8_t>(std::clamp(s.alpha, 0, 255)),
                          s.z);
    }
}

Status Stage::RestoreNodes(const std::vector<SavedNode>& nodes) {
    if (nodes.size() > 4'096) {
        return Status::Fail(diag::Diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXSTAGE7521",
            .category = "Runtime.Stage",
            .message = "Saved stage graph exceeds the node limit"});
    }
    const auto finite = [](const float value) { return std::isfinite(value); };
    std::unordered_map<std::string, Node> candidate;
    int nextOrder = 1;
    for (const SavedNode& saved : nodes) {
        const auto& t = saved.transform;
        const bool validKind = saved.kind == NodeKind::Group ||
                               saved.kind == NodeKind::Image ||
                               saved.kind == NodeKind::Character;
        if (!validKind || saved.name.empty() || saved.name.size() > 128 ||
            saved.parent.size() > 128 || !finite(t.x) || !finite(t.y) ||
            !finite(t.scaleX) || !finite(t.scaleY) || !finite(t.rotation) ||
            !finite(t.opacity) || t.scaleX <= 0.0f || t.scaleY <= 0.0f ||
            t.opacity < 0.0f || t.opacity > 1.0f ||
            saved.z < -kNodeOrderLimit || saved.z > kNodeOrderLimit ||
            saved.order < -kNodeOrderLimit ||
            saved.order > kNodeOrderLimit ||
            !candidate.emplace(saved.name,
                               Node{saved.kind, saved.parent, saved.children,
                                    saved.transform, saved.z, saved.order,
                                    saved.visible})
                 .second) {
            return Status::Fail(diag::Diagnostic{
                .severity = diag::Severity::Error,
                .code = "PXSTAGE7522",
                .category = "Runtime.Stage",
                .message = "Saved stage node is invalid",
                .details = saved.name});
        }
        nextOrder = std::max(nextOrder, saved.order + 1);
    }
    std::unordered_map<std::string, std::unordered_set<std::string>> derived;
    for (const auto& [name, node] : candidate) {
        if (!node.parent.empty()) {
            const auto parent = candidate.find(node.parent);
            if (parent == candidate.end() || parent->second.kind != NodeKind::Group ||
                !derived[node.parent].insert(name).second) {
                return Status::Fail(diag::Diagnostic{
                    .severity = diag::Severity::Error,
                    .code = "PXSTAGE7523",
                    .category = "Runtime.Stage",
                    .message = "Saved stage node parent is invalid",
                    .details = name});
            }
        }
    }
    for (const auto& [name, node] : candidate) {
        if (node.kind == NodeKind::Image && !m_layers.contains(name)) {
            return Status::Fail(diag::Diagnostic{
                .severity = diag::Severity::Error, .code = "PXSTAGE7524",
                .category = "Runtime.Stage",
                .message = "Saved image node has no layer payload", .details = name});
        }
        if (node.kind == NodeKind::Character && !m_actors.contains(name)) {
            return Status::Fail(diag::Diagnostic{
                .severity = diag::Severity::Error, .code = "PXSTAGE7525",
                .category = "Runtime.Stage",
                .message = "Saved character node has no actor payload", .details = name});
        }
        std::unordered_set<std::string> declared;
        for (const auto& child : node.children) {
            if (!declared.insert(child).second) {
                return Status::Fail(diag::Diagnostic{
                    .severity = diag::Severity::Error, .code = "PXSTAGE7526",
                    .category = "Runtime.Stage",
                    .message = "Saved stage node repeats a child", .details = name});
            }
        }
        if (declared != derived[name]) {
            return Status::Fail(diag::Diagnostic{
                .severity = diag::Severity::Error, .code = "PXSTAGE7527",
                .category = "Runtime.Stage",
                .message = "Saved stage parent/children links disagree",
                .details = name});
        }
        std::string cursor = node.parent;
        for (std::size_t depth = 0; !cursor.empty() && depth <= candidate.size();
             ++depth) {
            if (cursor == name) {
                return Status::Fail(diag::Diagnostic{
                    .severity = diag::Severity::Error, .code = "PXSTAGE7528",
                    .category = "Runtime.Stage",
                    .message = "Saved stage graph contains a cycle",
                    .details = name});
            }
            cursor = candidate.at(cursor).parent;
        }
        if (!cursor.empty()) {
            return Status::Fail(diag::Diagnostic{
                .severity = diag::Severity::Error, .code = "PXSTAGE7528",
                .category = "Runtime.Stage",
                .message = "Saved stage graph contains a cycle",
                .details = name});
        }
    }
    for (const auto& [name, _] : m_layers) {
        if (!candidate.contains(name) ||
            candidate.at(name).kind != NodeKind::Image)
            return Status::Fail(diag::Diagnostic{
                .severity = diag::Severity::Error, .code = "PXSTAGE7529",
                .category = "Runtime.Stage",
                .message = "Saved stage graph omits a layer", .details = name});
    }
    for (const auto& [name, _] : m_actors) {
        if (!candidate.contains(name) ||
            candidate.at(name).kind != NodeKind::Character)
            return Status::Fail(diag::Diagnostic{
                .severity = diag::Severity::Error, .code = "PXSTAGE7530",
                .category = "Runtime.Stage",
                .message = "Saved stage graph omits an actor", .details = name});
    }
    m_nodes = std::move(candidate);
    m_nextNodeOrder = nextOrder;
    return Status::Ok();
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
    state.nodes = SnapshotNodes();
    state.particleEmitters = m_particles.Capture();
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
        Node& node = EnsureNode(saved.name, NodeKind::Character);
        node.z = saved.slot;
    }
    RestoreLayers(state.layers);
    if (!state.nodes.empty()) {
        if (Status restored = RestoreNodes(state.nodes); !restored) return restored;
    }
    if (Status particles = m_particles.Restore(state.particleEmitters); !particles)
        return particles;
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
