#include "Engine/VN/Runtime/Stage.h"

#include "Engine/Animation/MotionRenderer.h"
#include "Engine/Diagnostics/Diagnostic.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Support/Easing.h"
#include "Engine/Support/Logger.h"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <ranges>
#include <unordered_set>

namespace px::vn {

namespace {
constexpr float kBgFadeSpeed = 2.5f;
// Longest a background change waits for its texture before fading anyway.
constexpr float kMaxBackgroundWait = 1.5f;
constexpr float kAlphaSpeed = 6.0f;
constexpr float kSlideSpeed = 9.0f;
constexpr float kSlideOffset = 40.0f;
constexpr int kNodeOrderLimit = 1'000'000;

float Approach(float value, float target, float rate, float dt) {
    const float t = std::min(1.0f, rate * dt);
    return value + (target - value) * t;
}

std::uint32_t EffectSeed(const std::string_view node,
                         const std::string_view effect) {
    std::uint32_t hash = 2166136261u;
    for (const auto text : {node, effect}) {
        for (const unsigned char character : text) {
            hash ^= character;
            hash *= 16777619u;
        }
        hash ^= 0xffu;
        hash *= 16777619u;
    }
    return hash;
}

diag::Diagnostic StageRestoreError(std::string code, std::string message,
                                   std::string details = {}) {
    diag::Diagnostic diagnostic;
    diagnostic.severity = diag::Severity::Error;
    diagnostic.code = std::move(code);
    diagnostic.category = "Runtime.Stage";
    diagnostic.message = std::move(message);
    diagnostic.details = std::move(details);
    return diagnostic;
}
}

Stage::Stage(graphics::Renderer2D& renderer, graphics::AssetCache& assets)
    : m_renderer(renderer), m_assets(assets), m_particles(renderer) {}

Stage::~Stage() {
    if (m_motion && m_motion->preloadBatch)
        (void)m_assets.CancelTexturePreload(m_motion->preloadBatch);
    EndRuleTransition();
}

Status Stage::PlayMotion(std::string path, const double speed,
                         const bool awaiting) {
    if (!path.ends_with(".pxmotion") || !std::isfinite(speed) ||
        speed <= 0.0 || speed > 16.0) {
        return Status::Fail(StageRestoreError(
            "PXSTAGE7520", "Stage motion path or speed is invalid", path));
    }
    const auto text = m_assets.ReadTextAsset(path);
    if (!text) return Status::Fail(StageRestoreError(
        "PXSTAGE7521", "Stage motion descriptor is unavailable", path));
    auto parsed = sdk::ParseMotion(*text, path);
    if (!parsed.Valid()) return Status::Fail(StageRestoreError(
        "PXSTAGE7522", "Stage motion descriptor is invalid", path));
    if (awaiting && parsed.clip->loop) return Status::Fail(StageRestoreError(
        "PXSTAGE7523", "Looping Stage motion cannot be awaited", path));
    const auto images = sdk::MotionDependencies(*parsed.clip);
    const auto audio = sdk::MotionAudioDependencies(*parsed.clip);
    for (const auto& dependency : images)
        if (!m_assets.ResourceExists(dependency))
            return Status::Fail(StageRestoreError(
                "PXSTAGE7524", "Stage motion image is missing", dependency));
    for (const auto& dependency : audio)
        if (!m_assets.ResourceExists(dependency))
            return Status::Fail(StageRestoreError(
                "PXSTAGE7525", "Stage motion audio is missing", dependency));
    auto next = std::make_unique<MotionPlayback>(
        std::move(path), std::move(*parsed.clip));
    next->speed = speed;
    next->awaiting = awaiting;
    StopMotion();
    if (!images.empty()) {
        std::vector<graphics::AssetPreloadRequest> requests;
        requests.reserve(images.size());
        for (const auto& image : images)
            requests.push_back({image, graphics::AssetPreloadPriority::Critical});
        next->preloadBatch = m_assets.BeginTexturePreload(std::move(requests));
    } else {
        next->ready = true;
    }
    m_motion = std::move(next);
    return Status::Ok();
}

void Stage::StopMotion() {
    if (!m_motion) return;
    const bool awaiting = m_motion->awaiting;
    if (m_motion->preloadBatch)
        (void)m_assets.CancelTexturePreload(m_motion->preloadBatch);
    m_motion.reset();
    if (awaiting && m_motionFinished) m_motionFinished();
}

void Stage::SetMotionCallbacks(
    std::function<void(const sdk::MotionAudioMarker&)> marker,
    std::function<void()> finished) {
    m_motionMarker = std::move(marker);
    m_motionFinished = std::move(finished);
}

bool Stage::MotionPlaying() const {
    return m_motion && m_motion->playing;
}

void Stage::EmitMotionMarkers(const double from, const double to,
                              const bool includeStart) {
    if (!m_motion || !m_motionMarker || m_motion->clip.markers.empty() ||
        to < from) return;
    const auto& clip = m_motion->clip;
    const auto lastCycle = clip.loop
        ? static_cast<std::uint64_t>(std::floor(to / clip.duration)) : 0;
    const auto startCycle = clip.loop
        ? static_cast<std::uint64_t>(std::floor(from / clip.duration)) : 0;
    const auto firstCycle = lastCycle > startCycle + 64
        ? lastCycle - 64 : startCycle;
    std::size_t emitted = 0;
    for (auto cycle = firstCycle; cycle <= lastCycle; ++cycle) {
        const auto base = static_cast<double>(cycle) * clip.duration;
        for (const auto& marker : clip.markers) {
            const auto at = base + marker.time;
            if ((at > from || (includeStart && at == from)) && at <= to) {
                m_motionMarker(marker);
                if (++emitted >= 256) return;
            }
        }
    }
}

void Stage::UpdateMotion(const float dt) {
    if (!m_motion || !m_motion->playing || !std::isfinite(dt) || dt <= 0.0f)
        return;
    auto& motion = *m_motion;
    if (!motion.ready) {
        const auto progress =
            m_assets.TexturePreloadProgress(motion.preloadBatch);
        if (!progress.valid || progress.failed != 0 ||
            progress.cancelled != 0) {
            diag::Emit(StageRestoreError(
                "PXSTAGE7526", "Stage motion image preload failed",
                motion.path));
            StopMotion();
            return;
        }
        if (!progress.Finished()) return;
        motion.ready = true;
    }
    const double from = motion.position;
    const double next = from + static_cast<double>(dt) * motion.speed;
    if (!std::isfinite(next)) return;
    if (motion.clip.loop && next / motion.clip.duration > 1'000'000.0) {
        motion.position = std::fmod(next, motion.clip.duration);
        motion.sampler.Seek(motion.position);
        motion.startMarkersPending = false;
        return;
    }
    const bool complete = !motion.clip.loop && next >= motion.clip.duration;
    const double endpoint = complete ? motion.clip.duration : next;
    EmitMotionMarkers(from, endpoint, motion.startMarkersPending);
    motion.position = motion.clip.loop
        ? std::fmod(endpoint, motion.clip.duration) : endpoint;
    motion.sampler.Seek(motion.position);
    motion.startMarkersPending = false;
    if (complete) {
        motion.playing = false;
        const bool awaiting = motion.awaiting;
        motion.awaiting = false;
        if (awaiting && m_motionFinished) m_motionFinished();
    }
}

const Stage::LayeredPortrait* Stage::ResolveLayeredPortrait(
    const std::string& path) {
    if(!path.ends_with(".pxportrait"))return nullptr;
    const auto assetSession=m_assets.AssetSession();
    if(m_portraitAssetSession!=assetSession){
        m_layeredPortraits.clear();
        m_invalidLayeredPortraits.clear();
        m_portraitAssetSession=assetSession;
    }
    if(const auto found=m_layeredPortraits.find(path);
       found!=m_layeredPortraits.end())return &found->second;
    if(m_invalidLayeredPortraits.contains(path))return nullptr;
    const auto text=m_assets.ReadTextAsset(path);
    auto portrait=text ? sdk::ParseLayeredPortrait(*text) : std::nullopt;
    if(!portrait){
        m_invalidLayeredPortraits[path]=true;
        PX_LOG_WARN("Stage: invalid layered portrait descriptor '{}'",path);
        return nullptr;
    }
    for(const auto& layer:portrait->layers)
        m_assets.PreloadTexture(layer.path,
            graphics::AssetPreloadPriority::High);
    return &m_layeredPortraits.emplace(path,std::move(*portrait)).first->second;
}

void Stage::PreloadCharacter(const std::string& imagePath) {
    if(imagePath.empty())return;
    if(!imagePath.ends_with(".pxportrait")){
        m_assets.PreloadTexture(imagePath,graphics::AssetPreloadPriority::High);
        return;
    }
    if(const auto* portrait=ResolveLayeredPortrait(imagePath)){
        for(const auto& layer:portrait->layers)
            m_assets.PreloadTexture(layer.path,graphics::AssetPreloadPriority::High);
    }
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

bool Stage::BackgroundSettled(const std::string& path) {
    if (path.empty() || m_assets.TextureSettled(path)) return true;
    m_assets.PreloadTexture(path, graphics::AssetPreloadPriority::Critical);
    return m_assets.TextureSettled(path);
}

void Stage::SetBackground(const std::string& path, bool transition) {
    if (path == m_bgPath) {
        return;
    }
    EndRuleTransition();
    m_prevBgPath = m_bgPath;
    m_bgPath = path;
    m_bgWait = 0.0f;
    // An instant change still waits for the decode behind the old image
    // rather than flashing an empty frame; Update completes it at once.
    m_bgInstant = !transition || m_prevBgPath.empty();
    m_bgFade = m_prevBgPath.empty() ? 1.0f : 0.0f;
    if (!m_prevBgPath.empty() && !BackgroundSettled(path) && m_bgInstant) return;
    if (m_bgInstant) {
        m_bgFade = 1.0f;
        m_prevBgPath.clear();
        m_bgInstant = false;
    }
}

namespace {

// Resamples like DisplayMode::Fill: cover the stage and crop the overflow
// equally, so the dissolving image matches the background it replaces.
SDL_Surface* CoverStage(SDL_Surface* source, const int width, const int height) {
    if (source->w == width && source->h == height) return source;
    const float scale = std::max(static_cast<float>(width) / static_cast<float>(source->w),
                                 static_cast<float>(height) / static_cast<float>(source->h));
    const int cropW = std::clamp(static_cast<int>(std::lround(width / scale)), 1, source->w);
    const int cropH = std::clamp(static_cast<int>(std::lround(height / scale)), 1, source->h);
    const SDL_Rect crop{(source->w - cropW) / 2, (source->h - cropH) / 2, cropW, cropH};
    SDL_Surface* result = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    if (!result) return nullptr;
    (void)SDL_SetSurfaceBlendMode(source, SDL_BLENDMODE_NONE);
    if (!SDL_BlitSurfaceScaled(source, &crop, result, nullptr, SDL_SCALEMODE_LINEAR)) {
        SDL_DestroySurface(result);
        return nullptr;
    }
    return result;
}

}  // namespace

Stage::RulePreparation Stage::PrepareRuleTransition(
    const std::function<SDL_Surface*(const std::string&)>& decode,
    const std::string& oldPath, const std::string& rulePath, const int width,
    const int height, std::shared_ptr<const RuleMask> mask) {
    RulePreparation result;
    const auto fail = [&result](std::string message, const std::string& path) {
        result.error = std::move(message);
        result.errorPath = path;
        return std::move(result);
    };
    if (!mask) {
        SDL_Surface* rule = decode(rulePath);
        if (!rule) return fail("Rule transition mask could not be decoded", rulePath);
        SDL_Surface* scaled = rule->w == width && rule->h == height
            ? rule : SDL_ScaleSurface(rule, width, height, SDL_SCALEMODE_LINEAR);
        if (scaled != rule) SDL_DestroySurface(rule);
        if (!scaled) return fail("Rule transition mask could not be resampled", rulePath);
        auto built = std::make_shared<RuleMask>();
        built->w = width;
        built->h = height;
        built->values.resize(static_cast<std::size_t>(width) * height);
        built->rowMin.assign(static_cast<std::size_t>(height), 255);
        built->rowMax.assign(static_cast<std::size_t>(height), 0);
        const auto* pixels = static_cast<const std::uint8_t*>(scaled->pixels);
        for (int row = 0; row < height; ++row) {
            const std::uint8_t* line = pixels + static_cast<std::size_t>(row) * scaled->pitch;
            std::uint8_t* out = built->values.data() + static_cast<std::size_t>(row) * width;
            std::uint8_t low = 255, high = 0;
            for (int col = 0; col < width; ++col) {
                const std::uint8_t value = line[col * 4];  // R channel
                out[col] = value;
                low = std::min(low, value);
                high = std::max(high, value);
            }
            built->rowMin[static_cast<std::size_t>(row)] = low;
            built->rowMax[static_cast<std::size_t>(row)] = high;
        }
        SDL_DestroySurface(scaled);
        mask = std::move(built);
    }
    result.mask = std::move(mask);

    SDL_Surface* old = decode(oldPath);
    if (!old) return fail("Outgoing background could not be decoded", oldPath);
    SDL_Surface* covered = CoverStage(old, width, height);
    if (covered != old) SDL_DestroySurface(old);
    if (!covered) return fail("Outgoing background could not be resampled", oldPath);
    result.oldPixels.resize(static_cast<std::size_t>(width) * height * 4u);
    const auto* oldPx = static_cast<const std::uint8_t*>(covered->pixels);
    for (int row = 0; row < height; ++row) {
        std::memcpy(result.oldPixels.data() + static_cast<std::size_t>(row) * width * 4,
                    oldPx + static_cast<std::size_t>(row) * covered->pitch,
                    static_cast<std::size_t>(width) * 4);
    }
    SDL_DestroySurface(covered);
    return result;
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
    if (oldBackgroundPath.empty() || w <= 0 || h <= 0 ||
        !m_assets.ResourceExists(oldBackgroundPath) || !m_assets.ResourceExists(rulePath)) {
        SetBackground(newBgPath, true);
        return;
    }

    // Decoding two full-screen images used to stall the frame that started
    // the transition. Prepare them on a worker and keep drawing the old
    // background until the dissolve can begin.
    const std::string maskKey = rulePath + "@" + std::to_string(w) + "x" + std::to_string(h);
    std::shared_ptr<const RuleMask> cachedMask;
    for (auto it = m_ruleMasks.begin(); it != m_ruleMasks.end(); ++it) {
        if (it->first != maskKey) continue;
        cachedMask = it->second;
        std::rotate(m_ruleMasks.begin(), it, it + 1);
        break;
    }
    RuleState& rs = m_ruleState;
    try {
        rs.preparing = std::async(
            std::launch::async,
            [decode = m_assets.SurfaceDecoder(), oldBackgroundPath, rulePath, w, h,
             cachedMask]() {
                return PrepareRuleTransition(decode, oldBackgroundPath, rulePath, w, h,
                                             cachedMask);
            });
    } catch (const std::exception&) {
        rs = RuleState{};
        SetBackground(newBgPath, true);
        return;
    }
    rs.active = true;
    rs.ready = false;
    rs.w = w;
    rs.h = h;
    rs.oldBackgroundPath = oldBackgroundPath;
    rs.newBackgroundPath = newBgPath;
    rs.rulePath = rulePath;
    rs.progress = 0.0f;
    rs.duration = std::max(0.05f, static_cast<float>(durationMs) / 1000.0f);
    rs.vague = std::max(1, vague);

    // Until preparation finishes, the old background stays fully drawn above
    // the (not yet visible) new one.
    m_prevBgPath = oldBackgroundPath;
    m_bgPath = newBgPath;
    m_bgFade = 0.0f;
    m_bgInstant = false;
    m_bgWait = 0.0f;
    (void)BackgroundSettled(newBgPath);
}

void Stage::FinishRulePreparation(const bool wait) {
    RuleState& rs = m_ruleState;
    if (!rs.active || rs.ready || !rs.preparing.valid() ||
        (!wait && rs.preparing.wait_for(std::chrono::seconds(0)) != std::future_status::ready))
        return;
    RulePreparation prepared = rs.preparing.get();
    if (prepared.mask) {
        const std::string key = rs.rulePath + "@" + std::to_string(rs.w) + "x" +
                                std::to_string(rs.h);
        std::erase_if(m_ruleMasks, [&key](const auto& entry) { return entry.first == key; });
        m_ruleMasks.insert(m_ruleMasks.begin(), {key, prepared.mask});
        constexpr std::size_t kRuleMaskCacheEntries = 6;
        if (m_ruleMasks.size() > kRuleMaskCacheEntries) m_ruleMasks.resize(kRuleMaskCacheEntries);
    }
    if (prepared.error.empty()) {
        rs.texture = graphics::TextureResource::CreateStreaming(
            graphics::TextureBackend::SdlRenderer, m_renderer.Handle(),
            graphics::StreamingTextureFormat::Rgba32, rs.w, rs.h);
        if (!rs.texture) prepared.error = "Rule transition texture could not be created";
    }
    if (!prepared.error.empty()) {
        PX_LOG_WARN("Stage: {} '{}'; using a crossfade", prepared.error,
                    prepared.errorPath.empty() ? rs.rulePath : prepared.errorPath);
        const float progress = rs.progress;
        rs = RuleState{};
        // m_prevBgPath/m_bgPath already describe a crossfade from the old
        // background; continue from the equivalent point.
        m_bgFade = std::clamp(progress, 0.0f, 1.0f);
        return;
    }
    (void)rs.texture.SetAlphaBlend();
    (void)rs.texture.SetLinearSampling();
    rs.oldPixels = std::move(prepared.oldPixels);
    rs.mask = std::move(prepared.mask);
    rs.uploaded = false;
    rs.ready = true;
    // The new background draws underneath; the old one dissolves out on top.
    m_prevBgPath.clear();
    m_bgFade = 1.0f;
}

void Stage::EndRuleTransition() {
    // A worker still decoding cannot be cancelled; retire it instead of
    // blocking this frame in the future's destructor.
    if (m_ruleState.preparing.valid())
        m_retiredRulePreparations.push_back(std::move(m_ruleState.preparing));
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

bool Stage::SetNodeEffect(
    const std::string& name, const std::string_view effect,
    const float progress,
    const graphics::CustomEffectNamedParameters& parameters) {
    auto found = m_nodes.find(name);
    if (found == m_nodes.end() || !std::isfinite(progress) ||
        !m_renderer.HasCustomEffect(effect, "node"))
        return false;
    const auto resolved = parameters.empty()
                              ? m_renderer.CustomEffectDefaults(effect)
                              : m_renderer.ResolveCustomEffectParameters(
                                    effect, parameters);
    if (!resolved) return false;
    found->second.effect = std::string(effect);
    found->second.effectProgress = std::clamp(progress, 0.0f, 1.0f);
    found->second.effectSeed = EffectSeed(name, effect);
    found->second.effectParameters = *resolved;
    return true;
}

void Stage::ClearNodeEffect(const std::string_view name) {
    const auto found = m_nodes.find(std::string(name));
    if (found == m_nodes.end()) return;
    found->second.effect.clear();
    found->second.effectProgress = 0.0f;
    found->second.effectSeed = 0;
    found->second.effectParameters = {};
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
        SavedNode saved;
        saved.name = name;
        saved.kind = node.kind;
        saved.parent = node.parent;
        saved.children = node.children;
        saved.transform = node.transform;
        saved.z = node.z;
        saved.order = node.order;
        saved.visible = node.visible;
        saved.effect = node.effect;
        saved.effectProgress = node.effectProgress;
        saved.effectSeed = node.effectSeed;
        saved.effectParameters = node.effectParameters;
        out.push_back(std::move(saved));
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
    const std::string& name, const Vec2 leafPivot) const {
    WorldNodeTransform world;
    std::vector<std::pair<std::string, const Node*>> chain;
    std::string cursor = name;
    while (!cursor.empty() && chain.size() <= m_nodes.size()) {
        const auto found = m_nodes.find(cursor);
        if (found == m_nodes.end()) break;
        chain.emplace_back(cursor, &found->second);
        cursor = found->second.parent;
    }
    std::ranges::reverse(chain);
    for (std::size_t index = 0; index < chain.size(); ++index) {
        const Node& node = *chain[index].second;
        const float radians = node.transform.rotation * 0.017453292519943295f;
        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);
        const float localA = cosine * node.transform.scaleX;
        const float localB = sine * node.transform.scaleX;
        const float localC = -sine * node.transform.scaleY;
        const float localD = cosine * node.transform.scaleY;
        float localTx = node.transform.x;
        float localTy = node.transform.y;
        if (index + 1 == chain.size() &&
            (leafPivot.x != 0.0f || leafPivot.y != 0.0f)) {
            // Legacy image/actor rotation is centre-based. Express that as a
            // true affine local matrix before composing it with its parents.
            localTx += node.transform.scaleX * leafPivot.x -
                       (localA * leafPivot.x + localC * leafPivot.y);
            localTy += node.transform.scaleY * leafPivot.y -
                       (localB * leafPivot.x + localD * leafPivot.y);
        }
        const WorldNodeTransform parent = world;
        world.a = parent.a * localA + parent.c * localB;
        world.b = parent.b * localA + parent.d * localB;
        world.c = parent.a * localC + parent.c * localD;
        world.d = parent.b * localC + parent.d * localD;
        world.tx = parent.a * localTx + parent.c * localTy + parent.tx;
        world.ty = parent.b * localTx + parent.d * localTy + parent.ty;
        world.opacity *= node.transform.opacity;
        world.visible = world.visible && node.visible;
    }
    return world;
}

std::vector<std::string> Stage::ResolveNodeEffectPath(
    const std::string& name) const {
    std::vector<std::string> path;
    std::string cursor = name;
    for (std::size_t depth = 0; !cursor.empty() && depth <= m_nodes.size();
         ++depth) {
        const auto found = m_nodes.find(cursor);
        if (found == m_nodes.end()) break;
        if (!found->second.effect.empty()) path.push_back(cursor);
        cursor = found->second.parent;
    }
    std::ranges::reverse(path);
    return path;
}

Stage::NodeSortKey Stage::ResolveNodeSortKey(const std::string& name) const {
    NodeSortKey key;
    std::vector<std::pair<std::string, const Node*>> chain;
    std::string cursor = name;
    while (!cursor.empty() && chain.size() <= m_nodes.size()) {
        const auto found = m_nodes.find(cursor);
        if (found == m_nodes.end()) break;
        chain.emplace_back(cursor, &found->second);
        cursor = found->second.parent;
    }
    std::ranges::reverse(chain);
    bool stackingContext = false;
    std::int64_t cumulativeZ = 0;
    for (const auto& [nodeName, node] : chain) {
        cumulativeZ += node->z;
        key.path.emplace_back(node->z, node->order, nodeName);
        // An effected group is a stacking context: all descendants remain
        // contiguous so the group can be captured and shaded exactly once.
        if (!stackingContext && !node->effect.empty()) {
            key.z = cumulativeZ;
            stackingContext = true;
        }
    }
    if (!stackingContext) key.z = cumulativeZ;
    return key;
}

bool Stage::Animate(const std::string& target, const TweenSpec& spec) {
    if (target.empty()) {
        return false;
    }
    if (target == "$camera" && ((spec.hasScale &&
        (!std::isfinite(spec.scale) || spec.scale <= 0.0f)) ||
        (spec.hasX && !std::isfinite(spec.x)) ||
        (spec.hasY && !std::isfinite(spec.y))))
        return false;
    Tween tween;
    tween.name = target;
    tween.spec = spec;
    tween.duration = std::max(0.001f, static_cast<float>(spec.durationMs) / 1000.0f);

    if (target == "$camera") {
        tween.isLayer = false;
        tween.fromX = m_cameraX;
        tween.fromY = m_cameraY;
        tween.fromScale = m_cameraZoom;
        if (spec.durationMs <= 0) {
            if (spec.hasX) m_cameraX = spec.x;
            if (spec.hasY) m_cameraY = spec.y;
            if (spec.hasScale) m_cameraZoom = spec.scale;
            std::erase_if(m_tweens, [](const Tween& active) {
                return active.name == "$camera";
            });
            return true;
        }
    } else if (auto it = m_actors.find(target); it != m_actors.end()) {
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
        else if (m_renderer.HasCustomEffect(property, "stage")) {
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
        if (tween.name == "$camera") {
            if (spec.hasX) m_cameraX = support::Lerp(tween.fromX, spec.x, t);
            if (spec.hasY) m_cameraY = support::Lerp(tween.fromY, spec.y, t);
            if (spec.hasScale) m_cameraZoom = support::Lerp(tween.fromScale, spec.scale, t);
        } else if (!tween.isLayer) {
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
        actor.alpha = actor.targetAlpha;
        actor.prevImagePath.clear();
        actor.prevAlpha = 0.0f;
    }
}

void Stage::SetCharacterAuto(const std::string& name, const std::string& imagePath,
                             const int slot, const float offsetX, const float offsetY,
                             const float scale) {
    if(name.empty())return;
    const auto found=m_actors.find(name);
    bool transition=found==m_actors.end()||found->second.imagePath.empty();
    if(!transition && found->second.imagePath!=imagePath){
        const auto* previous=ResolveLayeredPortrait(found->second.imagePath);
        const auto* current=ResolveLayeredPortrait(imagePath);
        if(previous && current && previous->schemaRevision==2 &&
           current->schemaRevision==2 && previous->width==current->width &&
           previous->height==current->height &&
           previous->layers.size()==current->layers.size()){
            bool hasBase=false;
            bool changedExpression=false;
            bool compatible=true;
            for(std::size_t index=0;index<previous->layers.size();++index){
                const auto& left=previous->layers[index];
                const auto& right=current->layers[index];
                if(left.role!=right.role){compatible=false;break;}
                const bool unchanged=left.path==right.path && left.x==right.x &&
                    left.y==right.y && left.width==right.width &&
                    left.height==right.height && left.opacity==right.opacity;
                if(left.role==PortraitLayer::Role::Base){
                    hasBase=true;
                    if(!unchanged){compatible=false;break;}
                }else if(!unchanged)changedExpression=true;
            }
            transition=compatible && hasBase && changedExpression;
        }
    }
    SetCharacter(name,imagePath,slot,transition,offsetX,offsetY,scale);
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

void Stage::ClearCharacters(const bool transition) {
    if (transition) {
        for (auto& entry : m_actors) {
            auto& actor = entry.second;
            actor.exiting = true;
            actor.targetAlpha = 0.0f;
        }
        return;
    }
    std::vector<std::string> names;
    names.reserve(m_actors.size());
    for (const auto& entry : m_actors) names.push_back(entry.first);
    for (const auto& name : names) RemoveNode(name);
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
    if (m_motion && m_motion->preloadBatch)
        (void)m_assets.CancelTexturePreload(m_motion->preloadBatch);
    m_motion.reset();
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
    m_bgInstant = false;
    m_bgWait = 0.0f;
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
    std::erase_if(m_tweens, [](const Tween& active) {
        return active.name == "$camera";
    });
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
    if (!m_renderer.HasCustomEffect(effect, "stage") ||
        !std::isfinite(progress))
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

    std::erase_if(m_retiredRulePreparations, [](const std::future<RulePreparation>& work) {
        return work.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    });
    if (m_ruleState.active && !m_ruleState.ready) FinishRulePreparation();
    // Fades hold while the incoming texture decodes (bounded, so a missing
    // decode result can never freeze the Stage) instead of popping in
    // partway through.
    const bool preparingRule = m_ruleState.active && !m_ruleState.ready;
    const bool incomingSettled = (m_bgFade >= 1.0f && !m_ruleState.active) ||
                                 BackgroundSettled(m_bgPath);
    const bool waiting = !incomingSettled && m_bgWait < kMaxBackgroundWait;
    if (waiting && (m_bgFade < 1.0f || m_ruleState.active)) m_bgWait += dt;

    if (m_bgFade < 1.0f && !preparingRule && !waiting) {
        m_bgFade = m_bgInstant ? 1.0f : std::min(1.0f, m_bgFade + kBgFadeSpeed * dt);
        if (m_bgFade >= 1.0f) {
            m_prevBgPath.clear();
            m_bgInstant = false;
        }
    }

    if (m_ruleState.active && m_ruleState.ready && !waiting) {
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
    UpdateMotion(dt);
}

void Stage::RenderRuleOverlay() {
    RuleState& rs = m_ruleState;
    if (!rs.active || !rs.ready || !rs.texture || !rs.mask) {
        return;
    }
    // Per-pixel alpha for the outgoing background: pixels whose rule value is
    // below the sweeping threshold fade out over a band `vague` wide. Alpha
    // depends only on the 8-bit rule value, so build it once per frame and
    // rewrite and upload only rows holding a value whose alpha changed.
    const float p = rs.progress * (255.0f + static_cast<float>(rs.vague));
    const float invVague = 255.0f / static_cast<float>(rs.vague);
    std::array<std::uint8_t, 256> alpha{};
    int changedLow = 256;
    int changedHigh = -1;
    for (int value = 0; value < 256; ++value) {
        const float a = (static_cast<float>(value) + static_cast<float>(rs.vague) - p) *
                        invVague;
        alpha[static_cast<std::size_t>(value)] =
            static_cast<std::uint8_t>(std::clamp(a, 0.0f, 255.0f));
        if (!rs.uploaded ||
            alpha[static_cast<std::size_t>(value)] != rs.alphaByRule[static_cast<std::size_t>(value)]) {
            changedLow = std::min(changedLow, value);
            changedHigh = std::max(changedHigh, value);
        }
    }
    if (changedHigh >= 0) {
        const RuleMask& mask = *rs.mask;
        const std::size_t width = static_cast<std::size_t>(rs.w);
        int firstRow = -1;
        int lastRow = -1;
        for (int row = 0; row < rs.h; ++row) {
            const auto index = static_cast<std::size_t>(row);
            if (rs.uploaded && (mask.rowMax[index] < changedLow || mask.rowMin[index] > changedHigh))
                continue;
            const std::uint8_t* values = mask.values.data() + index * width;
            std::uint8_t* pixels = rs.oldPixels.data() + index * width * 4u;
            for (std::size_t col = 0; col < width; ++col)
                pixels[col * 4u + 3u] = alpha[values[col]];
            if (firstRow < 0) firstRow = row;
            lastRow = row;
        }
        if (firstRow >= 0) {
            (void)rs.texture.UpdateRows(
                rs.oldPixels.data() + static_cast<std::size_t>(firstRow) * width * 4u,
                width * 4u, firstRow, lastRow - firstRow + 1);
        }
        rs.alphaByRule = alpha;
        rs.uploaded = true;
    }
    m_renderer.DrawTexture(rs.texture.Handle(),
                           Rect{0.0f, 0.0f, static_cast<float>(rs.w),
                                static_cast<float>(rs.h)});
}

void Stage::Render() {
    const bool compositing = m_renderer.BeginStageLayer();
    int logicalW = 0, logicalH = 0;
    m_renderer.GetLogicalSize(logicalW, logicalH);
    m_renderer.PushTransform(
        {static_cast<float>(logicalW) * 0.5f,
         static_cast<float>(logicalH) * 0.5f},
        {m_cameraZoom, m_cameraZoom});
    // Progress only moves past zero once prepared, unless a restore or a
    // timeline set it directly; that frame must show the exact dissolve.
    if (m_ruleState.active && !m_ruleState.ready && m_ruleState.progress > 0.0f)
        FinishRulePreparation(true);
    if (!m_prevBgPath.empty() && m_bgFade < 1.0f) {
        m_renderer.DrawImageAuto(m_prevBgPath, graphics::DisplayMode::Fill, 255);
    }
    if (!m_bgPath.empty()) {
        const auto alpha = static_cast<std::uint8_t>(std::clamp(m_bgFade, 0.0f, 1.0f) * 255.0f);
        m_renderer.DrawImageAuto(m_bgPath, graphics::DisplayMode::Fill, alpha);
    }
    RenderRuleOverlay();

    enum class DrawableKind : std::uint8_t { Layer, Actor, Particle };
    struct Drawable {
        DrawableKind kind = DrawableKind::Layer;
        std::string name;
        const Layer* layer = nullptr;
        const Actor* actor = nullptr;
        const ParticleEmitterState* emitter = nullptr;
        NodeSortKey key;
        std::vector<std::string> effects;
        WorldNodeTransform world;
        Rect localBounds;
        Rect worldBounds;
    };

    const auto transformBounds = [](const WorldNodeTransform& transform,
                                    const Rect& rect) {
        const auto point = [&transform](const float x, const float y) {
            return Vec2{transform.a * x + transform.c * y + transform.tx,
                        transform.b * x + transform.d * y + transform.ty};
        };
        const std::array<Vec2, 4> points{
            point(rect.x, rect.y), point(rect.x + rect.w, rect.y),
            point(rect.x + rect.w, rect.y + rect.h),
            point(rect.x, rect.y + rect.h)};
        float left = points.front().x;
        float right = left;
        float top = points.front().y;
        float bottom = top;
        for (const auto& value : points) {
            left = std::min(left, value.x);
            right = std::max(right, value.x);
            top = std::min(top, value.y);
            bottom = std::max(bottom, value.y);
        }
        return Rect{left, top, right - left, bottom - top};
    };
    const auto mergeBounds = [](const Rect& left, const Rect& right) {
        if (left.w <= 0.0f || left.h <= 0.0f) return right;
        if (right.w <= 0.0f || right.h <= 0.0f) return left;
        const float x = std::min(left.x, right.x);
        const float y = std::min(left.y, right.y);
        const float farX = std::max(left.x + left.w, right.x + right.w);
        const float farY = std::max(left.y + left.h, right.y + right.h);
        return Rect{x, y, farX - x, farY - y};
    };

    const auto emitters = m_particles.Capture();
    std::vector<Drawable> drawables;
    drawables.reserve(m_layers.size() + m_actors.size() + emitters.size());
    for (const auto& [name, layer] : m_layers) {
        const graphics::TextureHandle texture = m_assets.Texture(layer.imagePath);
        int textureWidth = 0;
        int textureHeight = 0;
        graphics::AssetCache::TextureSize(texture, textureWidth, textureHeight);
        if (!texture || textureWidth <= 0 || textureHeight <= 0) continue;
        Drawable drawable;
        drawable.kind = DrawableKind::Layer;
        drawable.name = name;
        drawable.layer = &layer;
        drawable.key = m_nodes.contains(name)
                           ? ResolveNodeSortKey(name)
                           : NodeSortKey{
                                 layer.z, {{layer.z, 0, name}}};
        drawable.effects = ResolveNodeEffectPath(name);
        drawable.localBounds =
            {0.0f, 0.0f, static_cast<float>(textureWidth),
             static_cast<float>(textureHeight)};
        drawable.world = ResolveNodeTransform(
            name, {textureWidth * 0.5f, textureHeight * 0.5f});
        if (!drawable.world.visible || drawable.world.opacity <= 0.0f)
            continue;
        drawable.worldBounds =
            transformBounds(drawable.world, drawable.localBounds);
        drawables.push_back(std::move(drawable));
    }

    const auto actorImageBounds = [&](const std::string& name,
                                      const Actor& actor,
                                      const std::string& path) {
        Rect empty;
        float sourceWidth=0.0f,sourceHeight=0.0f;
        if(const auto* portrait=ResolveLayeredPortrait(path)){
            sourceWidth=portrait->width;
            sourceHeight=portrait->height;
        }else{
            if(path.ends_with(".pxportrait"))return empty;
            const graphics::TextureHandle texture=m_assets.Texture(path);
            int textureWidth=0,textureHeight=0;
            graphics::AssetCache::TextureSize(texture,textureWidth,textureHeight);
            if(!texture||textureWidth<=0||textureHeight<=0)return empty;
            sourceWidth=static_cast<float>(textureWidth);
            sourceHeight=static_cast<float>(textureHeight);
        }
        const float effectScale = std::max(0.01f, actor.effectScale);
        const float width = sourceWidth * actor.scale * effectScale;
        const float height = sourceHeight * actor.scale * effectScale;
        const Rect local{
            actor.x - width * 0.5f + actor.offsetX + actor.effectOffsetX,
            static_cast<float>(logicalH) - height + actor.offsetY +
                actor.effectOffsetY,
            width, height};
        const auto world = ResolveNodeTransform(
            name, {local.x + local.w * 0.5f,
                   local.y + local.h * 0.5f});
        if (!world.visible || world.opacity <= 0.0f) return empty;
        return transformBounds(world, local);
    };
    for (const auto& [name, actor] : m_actors) {
        Drawable drawable;
        drawable.kind = DrawableKind::Actor;
        drawable.name = name;
        drawable.actor = &actor;
        drawable.key = m_nodes.contains(name)
                           ? ResolveNodeSortKey(name)
                           : NodeSortKey{
                                 actor.slot, {{actor.slot, 0, name}}};
        drawable.effects = ResolveNodeEffectPath(name);
        drawable.worldBounds = actorImageBounds(name, actor, actor.imagePath);
        if (!actor.prevImagePath.empty()) {
            drawable.worldBounds = mergeBounds(
                drawable.worldBounds,
                actorImageBounds(name, actor, actor.prevImagePath));
        }
        if (drawable.worldBounds.w <= 0.0f ||
            drawable.worldBounds.h <= 0.0f)
            continue;
        drawables.push_back(std::move(drawable));
    }

    for (const auto& emitter : emitters) {
        Drawable drawable;
        drawable.kind = DrawableKind::Particle;
        drawable.name = emitter.name;
        drawable.emitter = &emitter;
        drawable.key = {emitter.spec.z,
                        {{emitter.spec.z, kNodeOrderLimit, emitter.name}}};
        drawables.push_back(std::move(drawable));
    }

    std::ranges::sort(drawables, [](const Drawable& left,
                                    const Drawable& right) {
        if (left.key.z != right.key.z) return left.key.z < right.key.z;
        if (left.key.path != right.key.path)
            return left.key.path < right.key.path;
        if (left.kind != right.kind) return left.kind < right.kind;
        return left.name < right.name;
    });

    std::unordered_map<std::string, Rect> effectBounds;
    for (const auto& drawable : drawables) {
        if (drawable.kind == DrawableKind::Particle) continue;
        for (const auto& effect : drawable.effects) {
            effectBounds[effect] = mergeBounds(effectBounds[effect],
                                               drawable.worldBounds);
        }
    }

    struct ActiveEffect {
        std::string name;
        const Node* node = nullptr;
        Rect viewport;
        bool composed = false;
        bool direct = false;
    };
    std::vector<ActiveEffect> activeEffects;
    const auto closeEffect = [&] {
        ActiveEffect effect = std::move(activeEffects.back());
        activeEffects.pop_back();
        if (effect.composed) {
            m_renderer.EndComposition(
                effect.node->effect, effect.node->effectProgress,
                static_cast<float>(effect.node->effectSeed),
                effect.node->effectParameters, effect.viewport);
        } else if (effect.direct) {
            m_renderer.EndNodeEffect();
        }
    };
    const auto alignEffects = [&](const std::vector<std::string>& path) {
        std::size_t common = 0;
        while (common < activeEffects.size() && common < path.size() &&
               activeEffects[common].name == path[common])
            ++common;
        while (activeEffects.size() > common) closeEffect();
        for (std::size_t index = common; index < path.size(); ++index) {
            const auto found = m_nodes.find(path[index]);
            if (found == m_nodes.end() || found->second.effect.empty()) continue;
            Rect viewport{0.0f, 0.0f, static_cast<float>(logicalW),
                          static_cast<float>(logicalH)};
            if (const auto bounds = effectBounds.find(path[index]);
                bounds != effectBounds.end() && bounds->second.w > 0.0f &&
                bounds->second.h > 0.0f)
                viewport = bounds->second;
            // Leaf effects retain the established shader contract: the
            // source texture and UVs are the leaf itself. Only groups capture
            // their descendants into one source texture before shading.
            const bool composed = found->second.kind == NodeKind::Group &&
                                  m_renderer.BeginComposition();
            const bool direct =
                !composed && m_renderer.BeginNodeEffect(
                                 found->second.effect,
                                 found->second.effectProgress,
                                 static_cast<float>(found->second.effectSeed),
                                 found->second.effectParameters, viewport);
            activeEffects.push_back(
                {path[index], &found->second, viewport, composed, direct});
        }
    };

    const auto drawActorImage = [&](const std::string& name,
                                     const std::string& path,
                                     const Actor& actor, const float alpha,
                                     const std::size_t firstLayer = 0,
                                     const std::size_t layerLimit =
                                         (std::numeric_limits<std::size_t>::max)()) {
        const LayeredPortrait* portrait=ResolveLayeredPortrait(path);
        float sourceWidth=0.0f,sourceHeight=0.0f;
        if(portrait){
            sourceWidth=portrait->width;
            sourceHeight=portrait->height;
        }else{
            if(path.ends_with(".pxportrait"))return;
            const graphics::TextureHandle texture=m_assets.Texture(path);
            int textureWidth=0,textureHeight=0;
            graphics::AssetCache::TextureSize(texture,textureWidth,textureHeight);
            if(!texture||textureWidth<=0||textureHeight<=0)return;
            sourceWidth=static_cast<float>(textureWidth);
            sourceHeight=static_cast<float>(textureHeight);
        }
        const float effectScale = std::max(0.01f, actor.effectScale);
        const float spriteScale=actor.scale*effectScale;
        const float width=sourceWidth*spriteScale;
        const float height=sourceHeight*spriteScale;
        const Rect local{
            actor.x - width * 0.5f + actor.offsetX + actor.effectOffsetX,
            static_cast<float>(logicalH) - height + actor.offsetY +
                actor.effectOffsetY,
            width, height};
        const auto world = ResolveNodeTransform(
            name, {local.x + local.w * 0.5f,
                   local.y + local.h * 0.5f});
        if (!world.visible || world.opacity <= 0.0f) return;
        m_renderer.PushAffineTransform(
            {world.a, world.b, world.c, world.d, world.tx, world.ty});
        const float combinedAlpha=std::clamp(
            alpha*actor.effectAlpha*world.opacity,0.0f,255.0f);
        if(portrait){
            for(std::size_t index=firstLayer;
                index<portrait->layers.size() && index<layerLimit; ++index){
                const auto& layer=portrait->layers[index];
                m_renderer.DrawImage(
                    layer.path,
                    {local.x+layer.x*spriteScale,
                     local.y+layer.y*spriteScale,
                     layer.width*spriteScale,
                     layer.height*spriteScale},
                    static_cast<std::uint8_t>(std::clamp(
                        combinedAlpha*static_cast<float>(layer.opacity)/255.0f,
                        0.0f,255.0f)));
            }
        }else{
            m_renderer.DrawImage(path,local,
                static_cast<std::uint8_t>(combinedAlpha));
        }
        m_renderer.PopTransform();
    };

    for (const Drawable& drawable : drawables) {
        alignEffects(drawable.effects);
        if (drawable.kind == DrawableKind::Layer) {
            m_renderer.PushAffineTransform(
                {drawable.world.a, drawable.world.b, drawable.world.c,
                 drawable.world.d, drawable.world.tx, drawable.world.ty});
            m_renderer.DrawImage(
                drawable.layer->imagePath, drawable.localBounds,
                static_cast<std::uint8_t>(
                    std::clamp(drawable.world.opacity, 0.0f, 1.0f) *
                    255.0f));
            m_renderer.PopTransform();
        } else if (drawable.kind == DrawableKind::Actor) {
            if (!drawable.actor->prevImagePath.empty()) {
                const auto* previous = ResolveLayeredPortrait(
                    drawable.actor->prevImagePath);
                const auto* current = ResolveLayeredPortrait(
                    drawable.actor->imagePath);
                if (previous && current &&
                    previous->width == current->width &&
                    previous->height == current->height &&
                    previous->layers.size() == current->layers.size()) {
                    // Preserve every unchanged layer, including overlays
                    // after a changed face layer. Only differing layers fade.
                    for (std::size_t index = 0; index < current->layers.size(); ++index) {
                        const auto& left = previous->layers[index];
                        const auto& right = current->layers[index];
                        const bool unchanged = left.path == right.path &&
                            left.x == right.x && left.y == right.y &&
                            left.width == right.width &&
                            left.height == right.height &&
                            left.opacity == right.opacity;
                        if (unchanged) {
                            drawActorImage(drawable.name, drawable.actor->imagePath,
                                           *drawable.actor, drawable.actor->targetAlpha,
                                           index, index + 1);
                        } else {
                            drawActorImage(drawable.name,
                                           drawable.actor->prevImagePath,
                                           *drawable.actor, drawable.actor->prevAlpha,
                                           index, index + 1);
                            drawActorImage(drawable.name, drawable.actor->imagePath,
                                           *drawable.actor, drawable.actor->alpha,
                                           index, index + 1);
                        }
                    }
                } else if (previous && current &&
                           previous->width == current->width &&
                           previous->height == current->height) {
                    std::size_t sharedLayers = 0;
                    const auto limit = std::min(previous->layers.size(),
                                                current->layers.size());
                    while (sharedLayers < limit) {
                        const auto& left = previous->layers[sharedLayers];
                        const auto& right = current->layers[sharedLayers];
                        if (left.path != right.path || left.x != right.x ||
                            left.y != right.y || left.width != right.width ||
                            left.height != right.height ||
                            left.opacity != right.opacity) break;
                        ++sharedLayers;
                    }
                    if (sharedLayers > 0) {
                        drawActorImage(drawable.name, drawable.actor->imagePath,
                                       *drawable.actor, drawable.actor->targetAlpha,
                                       0, sharedLayers);
                    }
                    drawActorImage(drawable.name,
                                   drawable.actor->prevImagePath,
                                   *drawable.actor, drawable.actor->prevAlpha,
                                   sharedLayers);
                    drawActorImage(drawable.name, drawable.actor->imagePath,
                                   *drawable.actor, drawable.actor->alpha,
                                   sharedLayers);
                } else {
                    drawActorImage(drawable.name,
                                   drawable.actor->prevImagePath,
                                   *drawable.actor,
                                   drawable.actor->prevAlpha);
                    drawActorImage(drawable.name, drawable.actor->imagePath,
                                   *drawable.actor, drawable.actor->alpha);
                }
            } else {
                drawActorImage(drawable.name, drawable.actor->imagePath,
                               *drawable.actor, drawable.actor->alpha);
            }
        } else {
            const auto particles = m_particles.Samples(
                drawable.name, logicalW, logicalH);
            std::vector<graphics::SpriteBatchItem> batch;
            batch.reserve(particles.size());
            for (const auto& particle : particles) {
                graphics::SpriteBatchItem item;
                item.source = particle.source;
                item.destination = {particle.position.x, particle.position.y,
                                    particle.extent.x, particle.extent.y};
                item.color = particle.color;
                item.rotation = particle.rotation;
                item.sourceNormalized = !particle.texture.empty();
                batch.push_back(item);
            }
            m_renderer.DrawSpriteBatch(drawable.emitter->spec.texture, batch);
        }
    }
    while (!activeEffects.empty()) closeEffect();
    if (m_motion && m_motion->ready) {
        animation::DrawMotionPose(
            m_renderer, m_motion->clip, m_motion->sampler,
            {0.0f, 0.0f, static_cast<float>(logicalW),
             static_cast<float>(logicalH)});
    }
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
        graphics::StagePostEffects effects;
        effects.blur = effect("blur");
        effects.vignette = effect("vignette");
        effects.colorGrade = effect("color-grade");
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
        return Status::Fail(StageRestoreError(
            "PXSTAGE7521", "Saved stage graph exceeds the node limit"));
    }
    const auto finite = [](const float value) { return std::isfinite(value); };
    std::unordered_map<std::string, Node> candidate;
    int nextOrder = 1;
    for (const SavedNode& saved : nodes) {
        const auto& t = saved.transform;
        const bool validKind = saved.kind == NodeKind::Group ||
                               saved.kind == NodeKind::Image ||
                               saved.kind == NodeKind::Character;
        bool validEffect = std::isfinite(saved.effectProgress) &&
                           saved.effectProgress >= 0.0f &&
                           saved.effectProgress <= 1.0f &&
                           (saved.effect.empty() ||
                            m_renderer.HasCustomEffect(saved.effect, "node"));
        for (const auto& slot : saved.effectParameters)
            for (const float component : slot)
                validEffect = validEffect && std::isfinite(component);
        Node restored;
        restored.kind = saved.kind;
        restored.parent = saved.parent;
        restored.children = saved.children;
        restored.transform = saved.transform;
        restored.z = saved.z;
        restored.order = saved.order;
        restored.visible = saved.visible;
        restored.effect = saved.effect;
        restored.effectProgress = saved.effectProgress;
        restored.effectSeed = saved.effectSeed;
        restored.effectParameters = saved.effectParameters;
        if (!validKind || !validEffect || saved.name.empty() ||
            saved.name.size() > 128 ||
            saved.parent.size() > 128 || !finite(t.x) || !finite(t.y) ||
            !finite(t.scaleX) || !finite(t.scaleY) || !finite(t.rotation) ||
            !finite(t.opacity) || t.scaleX <= 0.0f || t.scaleY <= 0.0f ||
            t.opacity < 0.0f || t.opacity > 1.0f ||
            saved.z < -kNodeOrderLimit || saved.z > kNodeOrderLimit ||
            saved.order < -kNodeOrderLimit ||
            saved.order > kNodeOrderLimit ||
            !candidate.emplace(saved.name, std::move(restored))
                 .second) {
            return Status::Fail(StageRestoreError(
                "PXSTAGE7522", "Saved stage node is invalid", saved.name));
        }
        nextOrder = std::max(nextOrder, saved.order + 1);
    }
    std::unordered_map<std::string, std::unordered_set<std::string>> derived;
    for (const auto& [name, node] : candidate) {
        if (!node.parent.empty()) {
            const auto parent = candidate.find(node.parent);
            if (parent == candidate.end() || parent->second.kind != NodeKind::Group ||
                !derived[node.parent].insert(name).second) {
                return Status::Fail(StageRestoreError(
                    "PXSTAGE7523", "Saved stage node parent is invalid", name));
            }
        }
    }
    for (const auto& [name, node] : candidate) {
        if (node.kind == NodeKind::Image && !m_layers.contains(name)) {
            return Status::Fail(StageRestoreError(
                "PXSTAGE7524", "Saved image node has no layer payload", name));
        }
        if (node.kind == NodeKind::Character && !m_actors.contains(name)) {
            return Status::Fail(StageRestoreError(
                "PXSTAGE7525", "Saved character node has no actor payload", name));
        }
        std::unordered_set<std::string> declared;
        for (const auto& child : node.children) {
            if (!declared.insert(child).second) {
                return Status::Fail(StageRestoreError(
                    "PXSTAGE7526", "Saved stage node repeats a child", name));
            }
        }
        if (declared != derived[name]) {
            return Status::Fail(StageRestoreError(
                "PXSTAGE7527", "Saved stage parent/children links disagree",
                name));
        }
        std::string cursor = node.parent;
        for (std::size_t depth = 0; !cursor.empty() && depth <= candidate.size();
             ++depth) {
            if (cursor == name) {
                return Status::Fail(StageRestoreError(
                    "PXSTAGE7528", "Saved stage graph contains a cycle", name));
            }
            cursor = candidate.at(cursor).parent;
        }
        if (!cursor.empty()) {
            return Status::Fail(StageRestoreError(
                "PXSTAGE7528", "Saved stage graph contains a cycle", name));
        }
    }
    for (const auto& [name, _] : m_layers) {
        if (!candidate.contains(name) ||
            candidate.at(name).kind != NodeKind::Image)
            return Status::Fail(StageRestoreError(
                "PXSTAGE7529", "Saved stage graph omits a layer", name));
    }
    for (const auto& [name, _] : m_actors) {
        if (!candidate.contains(name) ||
            candidate.at(name).kind != NodeKind::Character)
            return Status::Fail(StageRestoreError(
                "PXSTAGE7530", "Saved stage graph omits an actor", name));
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
    if (m_motion) {
        state.motion = SavedMotion{m_motion->path, m_motion->position,
                                   m_motion->speed, m_motion->playing,
                                   m_motion->awaiting,
                                   m_motion->startMarkersPending};
    }
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
         !m_renderer.HasCustomEffect(state.customEffect, "stage"))) {
        return Status::Fail(StageRestoreError(
            "PXSTAGE7510", "Saved stage state contains invalid numbers"));
    }
    ClearAll();
    m_bgPath = state.background;
    m_prevBgPath = state.previousBackground;
    m_bgFade = std::clamp(state.backgroundFade, 0.0f, 1.0f);
    m_bgInstant = false;
    m_bgWait = 0.0f;
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
                return Status::Fail(StageRestoreError(
                    "PXSTAGE7510",
                    "Saved custom effect parameters contain invalid numbers"));
    for (const SavedActor& saved : state.actors) {
        if (saved.name.empty() || !finite(saved.alpha) || !finite(saved.targetAlpha) ||
            !finite(saved.previousAlpha) || !finite(saved.x) || !finite(saved.targetX) ||
            !finite(saved.offsetX) || !finite(saved.offsetY) || !finite(saved.scale) ||
            saved.scale <= 0.0f || !finite(saved.effectOffsetX) ||
            !finite(saved.effectOffsetY) || !finite(saved.effectScale) ||
            saved.effectScale <= 0.0f || !finite(saved.effectAlpha) ||
            saved.effectAlpha < 0.0f || saved.effectAlpha > 1.0f) {
            return Status::Fail(StageRestoreError(
                "PXSTAGE7511", "Saved actor state is invalid", saved.name));
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
            return Status::Fail(StageRestoreError(
                "PXSTAGE7512", "Saved stage animation is invalid", saved.target));
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
            return Status::Fail(StageRestoreError(
                "PXSTAGE7513", "Saved rule transition assets cannot be restored",
                state.ruleMask));
        }
        m_ruleState.progress = state.ruleProgress;
    }
    if (state.motion) {
        const auto& saved = *state.motion;
        if (!std::isfinite(saved.position) || saved.position < 0.0 ||
            (saved.awaiting && !saved.playing) ||
            (saved.startMarkersPending && saved.position != 0.0))
            return Status::Fail(StageRestoreError(
                "PXSTAGE7527", "Saved Stage motion state is invalid"));
        if (Status status = PlayMotion(saved.path, saved.speed, saved.awaiting);
            !status) return status;
        if ((!m_motion->clip.loop && saved.position > m_motion->clip.duration) ||
            (m_motion->clip.loop && saved.position >= m_motion->clip.duration) ||
            (!saved.playing &&
             (m_motion->clip.loop ||
              saved.position != m_motion->clip.duration)))
            return Status::Fail(StageRestoreError(
                "PXSTAGE7527", "Saved Stage motion time is out of range"));
        m_motion->position = saved.position;
        m_motion->sampler.Seek(saved.position);
        m_motion->playing = saved.playing;
        m_motion->startMarkersPending = saved.startMarkersPending;
    }
    return Status::Ok();
}

}
