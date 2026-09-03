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

    [[nodiscard]] std::vector<ParticleEmitterState> Capture() const;
    Status Restore(const std::vector<ParticleEmitterState>& state);
    [[nodiscard]] std::vector<ParticleSample> Samples(
        std::string_view name, int width, int height) const;

private:
    [[nodiscard]] static bool Valid(const ParticleEmitterSpec& spec);

    graphics::Renderer2D& m_renderer;
    std::unordered_map<std::string, ParticleEmitterState> m_emitters;
};

}  // namespace px::vn
