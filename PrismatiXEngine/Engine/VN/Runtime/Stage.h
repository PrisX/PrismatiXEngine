#pragma once

#include "Engine/Core/Variant.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/CustomEffect.h"
#include "Engine/Graphics/Texture.h"
#include "Engine/SDK/Motion.h"
#include "Engine/SDK/Portrait.h"
#include "Engine/VN/Runtime/ActorAction.h"
#include "Engine/VN/Runtime/ParticleSystem.h"

#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

struct SDL_Surface;

namespace px::graphics {
class Renderer2D;
class AssetCache;
}

namespace px::vn {

class Stage {
public:
    Stage(graphics::Renderer2D& renderer, graphics::AssetCache& assets);
    ~Stage();

    void SetBackground(const std::string& path, bool transition = true);
    // KAG-style universal transition: dissolves to newBgPath through a
    // grayscale rule image (dark areas switch first). Falls back to a plain
    // crossfade when the rule or old background cannot be loaded.
    void SetBackgroundRule(const std::string& newBgPath, const std::string& rulePath,
                           int durationMs, int vague);

    // Free-form image layers: z < 0 renders between the background and the
    // actors, z >= 0 in front of the actors.
    void SetLayer(const std::string& name, const std::string& imagePath, float x, float y,
                  float scale, std::uint8_t alpha, int z);
    void SetLayerTransform(const std::string& name, const std::string& imagePath,
                           float x, float y, float scaleX, float scaleY,
                           float rotation, std::uint8_t alpha, int z);
    void ClearLayer(const std::string& name);

    enum class NodeKind : std::uint8_t { Group, Image, Character };
    struct NodeTransform {
        float x = 0.0f;
        float y = 0.0f;
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        float rotation = 0.0f;
        float opacity = 1.0f;
    };
    struct SavedNode {
        std::string name;
        NodeKind kind = NodeKind::Group;
        std::string parent;
        std::vector<std::string> children;
        NodeTransform transform;
        int z = 0;
        int order = 0;
        bool visible = true;
        std::string effect;
        float effectProgress = 0.0f;
        std::uint32_t effectSeed = 0;
        std::array<std::array<float, 4>, 8> effectParameters{};
    };

    // Stage graph foundation. Legacy layers/characters are leaf nodes in this
    // graph, so SetLayer and ctx.stage.layer remain source-compatible.
    bool SetGroupNode(const std::string& name,
                      const std::string& parent = {});
    bool SetNodeParent(const std::string& name, const std::string& parent);
    bool SetNodeTransform(const std::string& name,
                          const NodeTransform& transform);
    bool SetNodeOrder(const std::string& name, int z, int order);
    bool SetNodeVisibility(const std::string& name, bool visible);
    bool SetNodeEffect(
        const std::string& name, std::string_view effect, float progress,
        const graphics::CustomEffectNamedParameters& parameters = {});
    void ClearNodeEffect(std::string_view name);
    void RemoveNode(const std::string& name);
    [[nodiscard]] std::vector<SavedNode> SnapshotNodes() const;

    bool SetParticleEmitter(const std::string& name,
                            const ParticleEmitterSpec& spec);
    void ClearParticleEmitter(std::string_view name);
    [[nodiscard]] std::vector<ParticleSample> SampleParticles(
        std::string_view name, int width, int height) const;

    // Tweened pose animation for an actor, a layer, or $camera. Only the
    // properties with has* set are animated. For actors x/y animate the
    // slot-relative offset; for $camera scale means zoom.
    struct TweenSpec {
        bool hasX = false;
        float x = 0.0f;
        bool hasY = false;
        float y = 0.0f;
        bool hasScale = false;
        float scale = 1.0f;
        bool hasAlpha = false;
        float alpha = 255.0f;
        int durationMs = 600;
        std::string ease = "outCubic";  // see px::support::Ease for names
    };
    bool Animate(const std::string& target, const TweenSpec& spec);
    // Timeline binding endpoint. Applies an already-sampled value immediately
    // to an actor/layer or to the camera target.
    bool ApplyAnimationProperty(const std::string& target, const std::string& property,
                                const Variant& value);
    // offsetX/offsetY shift the sprite from its slot anchor; scale resizes it.
    void SetCharacter(const std::string& name, const std::string& imagePath, int slot,
                      bool transition = true, float offsetX = 0.0f, float offsetY = 0.0f,
                      float scale = 1.0f);
    // Revision-2 portrait roles limit automatic replacement fades to facial
    // expression layers; new actors retain the ordinary entrance transition.
    void SetCharacterAuto(const std::string& name, const std::string& imagePath, int slot,
                          float offsetX = 0.0f, float offsetY = 0.0f,
                          float scale = 1.0f);
    // Current slot and authored placement of a visible (non-exiting) actor,
    // so an expression-only [show] can keep the character where it stands.
    struct CharacterPlacement {
        int slot = 2;
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        float scale = 1.0f;
    };
    [[nodiscard]] std::optional<CharacterPlacement> FindCharacterPlacement(
        const std::string& name) const;
    // Prefetches a plain sprite or the image layers referenced by a portrait
    // descriptor. Descriptors themselves are not image textures.
    void PreloadCharacter(const std::string& imagePath);
    // Plays a relative action preset (hop, nod, bow, ...) on a visible actor;
    // "stop" eases an active action back to rest. False for an unknown
    // actor, preset, or out-of-range parameters.
    bool PlayActorAction(const std::string& name, const ActorActionRequest& request);
    void ClearCharacter(const std::string& name, bool transition = true);
    // Clears character actors without resetting the rest of the stage.
    void ClearCharacters(bool transition = true);
    // Slides an existing actor to another slot without changing its image.
    void MoveCharacter(const std::string& name, int slot);
    void ClearAll();
    // A single full-canvas motion is composited above Stage actors and below
    // dialogue/UI. Replacing or stopping it retires its preload batch.
    Status PlayMotion(std::string path, double speed = 1.0,
                      bool awaiting = false);
    void StopMotion();
    void SetMotionCallbacks(
        std::function<void(const sdk::MotionAudioMarker&)> marker,
        std::function<void()> finished);
    [[nodiscard]] bool MotionPlaying() const;
    // Camera shake (KAG [quake]-style): durationMs of decaying jitter.
    void Shake(int durationMs, float amplitude);
    bool SetCamera(float x, float y, float zoom);
    bool SetScreenEffect(std::string_view effect, float amount);
    void ClearScreenEffect(std::string_view effect);
    bool SetCustomEffect(
        std::string_view effect, float progress,
        const std::array<std::array<float, 4>, 8>* parameters = nullptr);
    bool SetCustomEffect(std::string_view effect, float progress,
                         const graphics::CustomEffectNamedParameters& parameters);
    void ClearCustomEffect();

    void Update(float dt);
    void Render();

    struct SavedActor {
        std::string name;
        std::string imagePath;
        int slot = 2;
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        float scale = 1.0f;
        std::string previousImagePath;
        float alpha = 255.0f;
        float targetAlpha = 255.0f;
        float previousAlpha = 0.0f;
        float x = 0.0f;
        float targetX = 0.0f;
        bool exiting = false;
        float effectOffsetX = 0.0f;
        float effectOffsetY = 0.0f;
        float effectScale = 1.0f;
        float effectAlpha = 1.0f;
        // Active action, restored by replaying the request to this time.
        std::optional<ActorActionRequest> action;
        double actionElapsed = 0.0;
    };
    [[nodiscard]] std::vector<SavedActor> Snapshot() const;
    void Restore(const std::vector<SavedActor>& actors);

    struct SavedLayer {
        std::string name;
        std::string imagePath;
        float x = 0.0f;
        float y = 0.0f;
        float scale = 1.0f;
        int alpha = 255;
        int z = 0;
        float scaleY = 1.0f;
        float rotation = 0.0f;
    };
    [[nodiscard]] std::vector<SavedLayer> SnapshotLayers() const;
    void RestoreLayers(const std::vector<SavedLayer>& layers);

    struct SavedTween {
        bool layer = false;
        std::string target;
        TweenSpec spec;
        float fromX = 0.0f;
        float fromY = 0.0f;
        float fromScale = 1.0f;
        float fromAlpha = 255.0f;
        float elapsed = 0.0f;
        float duration = 0.6f;
    };

    struct SavedMotion {
        std::string path;
        double position = 0.0;
        double speed = 1.0;
        bool playing = false;
        bool awaiting = false;
        bool startMarkersPending = false;
    };

    // Serializable state of every visual value that affects the next rendered
    // frame. GPU textures are reconstructed from resource ids during restore.
    struct RuntimeState {
        std::string background;
        std::string previousBackground;
        float backgroundFade = 1.0f;
        bool ruleActive = false;
        std::string ruleOldBackground;
        std::string ruleNewBackground;
        std::string ruleMask;
        float ruleProgress = 0.0f;
        float ruleDuration = 0.6f;
        int ruleVague = 64;
        float cameraX = 0.0f;
        float cameraY = 0.0f;
        float cameraZoom = 1.0f;
        float shakeRemaining = 0.0f;
        float shakeDuration = 0.0f;
        float shakeAmplitude = 0.0f;
        float shakePhase = 0.0f;
        std::unordered_map<std::string, float> screenEffects;
        std::string customEffect;
        float customEffectProgress = 0.0f;
        std::uint32_t customEffectSeed = 0;
        std::uint32_t customEffectSequence = 0;
        std::array<std::array<float, 4>, 8> customEffectParameters{};
        std::vector<SavedActor> actors;
        std::vector<SavedLayer> layers;
        std::vector<SavedTween> tweens;
        std::vector<SavedNode> nodes;
        std::vector<ParticleEmitterState> particleEmitters;
        std::optional<SavedMotion> motion;
    };
    [[nodiscard]] RuntimeState CaptureState() const;
    Status RestoreState(const RuntimeState& state);

    [[nodiscard]] const std::string& BackgroundPath() const { return m_bgPath; }

private:
    using PortraitLayer = sdk::PortraitLayer;
    using LayeredPortrait = sdk::LayeredPortrait;
    [[nodiscard]] const LayeredPortrait* ResolveLayeredPortrait(
        const std::string& path);

    struct Actor {
        std::string imagePath;
        std::string prevImagePath;  // old expression fading out under the new one
        int slot = 2;
        float alpha = 0.0f;
        float targetAlpha = 255.0f;
        float prevAlpha = 0.0f;
        float x = 0.0f;
        float targetX = 0.0f;
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        float scale = 1.0f;
        float effectOffsetX = 0.0f;
        float effectOffsetY = 0.0f;
        float effectScale = 1.0f;
        float effectAlpha = 1.0f;
        bool exiting = false;
        std::optional<ActorActionRequest> actionRequest;
        ActorActionTrack actionTrack;
        double actionElapsed = 0.0;
        ActorPose actionPose;
    };

    struct Layer {
        std::string imagePath;
        float x = 0.0f;
        float y = 0.0f;
        float scale = 1.0f;
        float scaleY = 1.0f;
        float rotation = 0.0f;
        std::uint8_t alpha = 255;
        int z = 0;
    };

    // Grayscale rule image resampled to the stage size. Row extrema let a
    // frame skip rows whose pixels cannot change at the current threshold.
    struct RuleMask {
        int w = 0;
        int h = 0;
        std::vector<std::uint8_t> values;
        std::vector<std::uint8_t> rowMin;
        std::vector<std::uint8_t> rowMax;
    };

    struct RulePreparation {
        std::vector<std::uint8_t> oldPixels;  // RGBA of the outgoing background
        std::shared_ptr<const RuleMask> mask;
        std::string error;
        std::string errorPath;
    };

    struct RuleState {
        bool active = false;
        // Decoding runs off the frame thread; progress holds at zero until
        // the outgoing pixels and the streaming texture exist.
        bool ready = false;
        std::future<RulePreparation> preparing;
        std::string oldBackgroundPath;
        std::string newBackgroundPath;
        std::string rulePath;
        float progress = 0.0f;
        float duration = 0.6f;
        int vague = 64;
        int w = 0;
        int h = 0;
        std::vector<std::uint8_t> oldPixels;  // RGBA of the outgoing background
        std::shared_ptr<const RuleMask> mask;
        std::array<std::uint8_t, 256> alphaByRule{};
        bool uploaded = false;
        graphics::TextureResource texture;
    };

    struct Tween {
        bool isLayer = false;
        std::string name;
        TweenSpec spec;
        float fromX = 0.0f;
        float fromY = 0.0f;
        float fromScale = 1.0f;
        float fromAlpha = 255.0f;
        float elapsed = 0.0f;
        float duration = 0.6f;
    };

    struct MotionPlayback {
        std::string path;
        sdk::MotionClip clip;
        sdk::MotionSampler sampler;
        std::uint64_t preloadBatch = 0;
        double position = 0.0;
        double speed = 1.0;
        bool playing = true;
        bool awaiting = false;
        bool ready = false;
        bool startMarkersPending = true;
        MotionPlayback(std::string source, sdk::MotionClip value)
            : path(std::move(source)), clip(std::move(value)), sampler(clip) {}
    };

    struct Node {
        NodeKind kind = NodeKind::Group;
        std::string parent;
        std::vector<std::string> children;
        NodeTransform transform;
        int z = 0;
        int order = 0;
        bool visible = true;
        std::string effect;
        float effectProgress = 0.0f;
        std::uint32_t effectSeed = 0;
        std::array<std::array<float, 4>, 8> effectParameters{};
    };

    struct WorldNodeTransform {
        float a = 1.0f;
        float b = 0.0f;
        float c = 0.0f;
        float d = 1.0f;
        float tx = 0.0f;
        float ty = 0.0f;
        float opacity = 1.0f;
        bool visible = true;
    };

    struct NodeSortKey {
        std::int64_t z = 0;
        std::vector<std::tuple<int, int, std::string>> path;
    };

    [[nodiscard]] float SlotCenterX(int slot) const;
    void RenderRuleOverlay();
    void RenderScreenEffects();
    void EndRuleTransition();
    void FinishRulePreparation(bool wait = false);
    [[nodiscard]] static RulePreparation PrepareRuleTransition(
        const std::function<SDL_Surface*(const std::string&)>& decode,
        const std::string& oldPath, const std::string& rulePath, int width,
        int height, std::shared_ptr<const RuleMask> mask);
    // True once the texture has a decode outcome; requests it otherwise.
    bool BackgroundSettled(const std::string& path);
    void UpdateTweens(float dt);
    void UpdateMotion(float dt);
    void EmitMotionMarkers(double from, double to, bool includeStart);
    Node& EnsureNode(const std::string& name, NodeKind kind);
    [[nodiscard]] WorldNodeTransform ResolveNodeTransform(
        const std::string& name, Vec2 leafPivot = {}) const;
    [[nodiscard]] NodeSortKey ResolveNodeSortKey(
        const std::string& name) const;
    [[nodiscard]] std::vector<std::string> ResolveNodeEffectPath(
        const std::string& name) const;
    [[nodiscard]] bool WouldCreateNodeCycle(
        const std::string& name, const std::string& parent) const;
    Status RestoreNodes(const std::vector<SavedNode>& nodes);

    graphics::Renderer2D& m_renderer;
    graphics::AssetCache& m_assets;

    std::unordered_map<std::string, Layer> m_layers;
    std::unordered_map<std::string, Node> m_nodes;
    int m_nextNodeOrder = 1;
    ParticleSystem m_particles;
    std::vector<Tween> m_tweens;
    std::unique_ptr<MotionPlayback> m_motion;
    std::function<void(const sdk::MotionAudioMarker&)> m_motionMarker;
    std::function<void()> m_motionFinished;
    RuleState m_ruleState;
    // Recently used rule masks by "path@WxH"; transitions reuse a few rules.
    std::vector<std::pair<std::string, std::shared_ptr<const RuleMask>>> m_ruleMasks;
    std::vector<std::future<RulePreparation>> m_retiredRulePreparations;
    // Seconds a background change has waited for its texture decode.
    float m_bgWait = 0.0f;
    // A non-transition change waiting behind the old image for its decode.
    bool m_bgInstant = false;

    std::string m_bgPath;
    std::string m_prevBgPath;
    float m_bgFade = 1.0f;

    float m_shakeRemaining = 0.0f;
    float m_shakeDuration = 0.0f;
    float m_shakeAmplitude = 0.0f;
    float m_shakePhase = 0.0f;
    float m_cameraX = 0.0f;
    float m_cameraY = 0.0f;
    float m_cameraZoom = 1.0f;
    std::unordered_map<std::string,float> m_screenEffects;
    std::string m_customEffect;
    float m_customEffectProgress = 0.0f;
    std::uint32_t m_customEffectSeed = 0x6d2b79f5u;
    std::uint32_t m_customEffectSequence = 0;
    std::array<std::array<float, 4>, 8> m_customEffectParameters{};

    std::unordered_map<std::string, Actor> m_actors;
    std::unordered_map<std::string, LayeredPortrait> m_layeredPortraits;
    std::unordered_map<std::string, bool> m_invalidLayeredPortraits;
    std::uint64_t m_portraitAssetSession = 0;
};

}
