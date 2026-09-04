#pragma once

#include "Engine/Core/Variant.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/CustomEffect.h"
#include "Engine/Graphics/Texture.h"
#include "Engine/VN/Runtime/ParticleSystem.h"

#include <cstdint>
#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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

    // Tweened pose animation for an actor or a layer (matched by name; actors
    // take precedence). Only the properties with has* set are animated.
    // For actors x/y animate the slot-relative offset.
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
    void ClearCharacter(const std::string& name, bool transition = true);
    // Slides an existing actor to another slot without changing its image.
    void MoveCharacter(const std::string& name, int slot);
    void ClearAll();
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
    };
    [[nodiscard]] RuntimeState CaptureState() const;
    Status RestoreState(const RuntimeState& state);

    [[nodiscard]] const std::string& BackgroundPath() const { return m_bgPath; }

private:
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

    struct RuleState {
        bool active = false;
        std::string oldBackgroundPath;
        std::string newBackgroundPath;
        std::string rulePath;
        float progress = 0.0f;
        float duration = 0.6f;
        int vague = 64;
        int w = 0;
        int h = 0;
        std::vector<std::uint8_t> oldPixels;  // RGBA of the outgoing background
        std::vector<std::uint8_t> rule;       // grayscale rule per pixel
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
        float x = 0.0f;
        float y = 0.0f;
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        float rotation = 0.0f;
        float opacity = 1.0f;
        bool visible = true;
    };

    struct NodeSortKey {
        std::int64_t z = 0;
        std::vector<std::pair<int, std::string>> path;
    };

    [[nodiscard]] float SlotCenterX(int slot) const;
    void RenderLayers(bool front);
    void RenderRuleOverlay();
    void RenderScreenEffects();
    void EndRuleTransition();
    void UpdateTweens(float dt);
    Node& EnsureNode(const std::string& name, NodeKind kind);
    [[nodiscard]] WorldNodeTransform ResolveNodeTransform(
        const std::string& name) const;
    [[nodiscard]] NodeSortKey ResolveNodeSortKey(
        const std::string& name) const;
    [[nodiscard]] const Node* ResolveNodeEffect(
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
    RuleState m_ruleState;

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
};

}
