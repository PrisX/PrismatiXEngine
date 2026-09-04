#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::graphics {
class Renderer2D;
}

namespace px::vn {

enum class ParticlePreset : std::uint8_t {
    Rain,
    Snow,
    Sakura,
    Dust,
};

enum class ParticleSpawnShape : std::uint8_t { Point, Box, Line, Ellipse };
[[nodiscard]] std::string_view ParticleSpawnShapeName(ParticleSpawnShape shape);
[[nodiscard]] std::optional<ParticleSpawnShape> ParticleSpawnShapeFromName(
    std::string_view name);

struct ParticleRange {
    float minimum = 0.0f;
    float maximum = 0.0f;
};

struct ParticleCurvePoint {
    float time = 0.0f;
    float value = 1.0f;
};

struct ParticleColorPoint {
    float time = 0.0f;
    Color value{255, 255, 255, 255};
};

[[nodiscard]] std::string_view ParticlePresetName(ParticlePreset preset);
[[nodiscard]] std::optional<ParticlePreset> ParticlePresetFromName(
    std::string_view name);

struct ParticleEmitterSpec {
    ParticlePreset preset = ParticlePreset::Rain;
    std::uint32_t seed = 1;
    float rate = 48.0f;
    std::uint32_t maxParticles = 256;
    int z = 0;
    float opacity = 1.0f;
    float wind = 0.0f;
    float speed = 1.0f;
    float size = 1.0f;

    // Production declarative emitter contract. Position ranges are normalized
    // to the logical Stage viewport; motion values are logical pixels/second.
    std::string texture;
    std::uint32_t atlasColumns = 1;
    std::uint32_t atlasRows = 1;
    std::uint32_t atlasFirstFrame = 0;
    std::uint32_t atlasFrameCount = 1;
    ParticleSpawnShape spawnShape = ParticleSpawnShape::Box;
    ParticleRange positionX{0.0f, 1.0f};
    ParticleRange positionY{0.0f, 0.0f};
    ParticleRange velocityX{0.0f, 0.0f};
    ParticleRange velocityY{0.0f, 0.0f};
    ParticleRange accelerationX{0.0f, 0.0f};
    ParticleRange accelerationY{0.0f, 0.0f};
    ParticleRange lifetime{1.0f, 2.0f};
    ParticleRange rotation{0.0f, 0.0f};
    ParticleRange angularVelocity{0.0f, 0.0f};
    ParticleRange scale{1.0f, 1.0f};
    ParticleRange initialOpacity{1.0f, 1.0f};
    std::vector<ParticleCurvePoint> scaleOverLifetime;
    std::vector<ParticleCurvePoint> opacityOverLifetime;
    std::vector<ParticleColorPoint> colorOverLifetime;
    float gravity = 0.0f;
    float variation = 0.0f;
    std::uint32_t burst = 0;
    bool loop = true;
    float duration = 0.0f;
    bool advanced = false;
};

struct ParticleEmitterState {
    std::string name;
    ParticleEmitterSpec spec;
    std::uint64_t ticks = 0;
    double tickRemainder = 0.0;
};

struct ParticleSample {
    std::uint64_t id = 0;
    Vec2 position;
    Vec2 extent;
    Color color;
    float rotation = 0.0f;
    Rect source;
    std::string texture;
};

struct ParticleRenderStats {
    std::uint64_t particles = 0;
    std::uint32_t batches = 0;
};

// Fixed-step, stateless-per-particle simulation. Only emitter ticks are saved;
// every visible particle is reproduced from (seed, particle id, tick).
class ParticleSystem {
public:
    explicit ParticleSystem(graphics::Renderer2D& renderer)
        : m_renderer(renderer) {}

    bool Set(std::string name, const ParticleEmitterSpec& spec);
    void Clear(std::string_view name);
    void ClearAll();
    void Update(float deltaSeconds);
    void Render(bool front) const;
    [[nodiscard]] ParticleRenderStats LastRenderStats() const {
        return m_lastRenderStats;
    }

    [[nodiscard]] std::vector<ParticleEmitterState> Capture() const;
    Status Restore(const std::vector<ParticleEmitterState>& state);
    [[nodiscard]] std::vector<ParticleSample> Samples(
        std::string_view name, int width, int height) const;

private:
    [[nodiscard]] static bool Valid(const ParticleEmitterSpec& spec);

    graphics::Renderer2D& m_renderer;
    std::unordered_map<std::string, ParticleEmitterState> m_emitters;
    mutable ParticleRenderStats m_lastRenderStats;
};

}  // namespace px::vn
