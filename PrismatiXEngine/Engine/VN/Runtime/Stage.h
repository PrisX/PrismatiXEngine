#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_Texture;

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
    void ClearLayer(const std::string& name);

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

    void Update(float dt);
    void Render();

    struct SavedActor {
        std::string name;
        std::string imagePath;
        int slot = 2;
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        float scale = 1.0f;
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
    };
    [[nodiscard]] std::vector<SavedLayer> SnapshotLayers() const;
    void RestoreLayers(const std::vector<SavedLayer>& layers);

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
        bool exiting = false;
    };

    struct Layer {
        std::string imagePath;
        float x = 0.0f;
        float y = 0.0f;
        float scale = 1.0f;
        std::uint8_t alpha = 255;
        int z = 0;
    };

    struct RuleState {
        bool active = false;
        float progress = 0.0f;
        float duration = 0.6f;
        int vague = 64;
        int w = 0;
        int h = 0;
        std::vector<std::uint8_t> oldPixels;  // RGBA of the outgoing background
        std::vector<std::uint8_t> rule;       // grayscale rule per pixel
        SDL_Texture* texture = nullptr;
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

    [[nodiscard]] float SlotCenterX(int slot) const;
    void RenderLayers(bool front);
    void RenderRuleOverlay();
    void EndRuleTransition();
    void UpdateTweens(float dt);

    graphics::Renderer2D& m_renderer;
    graphics::AssetCache& m_assets;

    std::unordered_map<std::string, Layer> m_layers;
    std::vector<Tween> m_tweens;
    RuleState m_ruleState;

    std::string m_bgPath;
    std::string m_prevBgPath;
    float m_bgFade = 1.0f;

    float m_shakeRemaining = 0.0f;
    float m_shakeDuration = 0.0f;
    float m_shakeAmplitude = 0.0f;
    float m_shakePhase = 0.0f;

    std::unordered_map<std::string, Actor> m_actors;
};

}
