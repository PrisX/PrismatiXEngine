#include "Engine/VN/Runtime/ParticleSystem.h"

#include "Engine/Graphics/Renderer2D.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <unordered_set>

namespace px::vn {
namespace {

constexpr double kTicksPerSecond = 60.0;
constexpr std::uint64_t kMaxEmitterTicks =
    60ull * 60ull * 60ull * 24ull * 366ull * 100ull;

std::uint32_t Hash(std::uint32_t value) {
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

float Unit(const std::uint32_t seed, const std::uint64_t id,
           const std::uint32_t channel) {
    const auto folded = static_cast<std::uint32_t>(id) ^
                        static_cast<std::uint32_t>(id >> 32u);
    return static_cast<float>(Hash(seed ^ Hash(folded + channel * 0x9e3779b9u))) /
           static_cast<float>((std::numeric_limits<std::uint32_t>::max)());
}

Color PresetColor(const ParticlePreset preset, const float opacity) {
    const auto alpha = static_cast<std::uint8_t>(
        std::clamp(opacity, 0.0f, 1.0f) * 255.0f);
    switch (preset) {
        case ParticlePreset::Rain: return {174, 208, 255, alpha};
        case ParticlePreset::Snow: return {245, 250, 255, alpha};
        case ParticlePreset::Sakura: return {255, 174, 201, alpha};
        case ParticlePreset::Dust: return {255, 239, 184, alpha};
    }
    return {255, 255, 255, alpha};
}

bool SafeTexturePath(const std::string_view path) {
    if (path.empty()) return true;
    if (path.size() > 4096 || path.front() == '/' ||
        path.find('\\') != std::string_view::npos ||
        path.find(':') != std::string_view::npos)
        return false;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto part = path.substr(
            start, end == std::string_view::npos ? path.size() - start
                                                  : end - start);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

bool ValidRange(const ParticleRange& range, const float minimum,
                const float maximum) {
    return std::isfinite(range.minimum) && std::isfinite(range.maximum) &&
           range.minimum >= minimum && range.maximum <= maximum &&
           range.minimum <= range.maximum;
}

bool ValidCurve(const std::vector<ParticleCurvePoint>& curve,
                const float minimum, const float maximum) {
    if (curve.size() > 8) return false;
    float previous = -1.0f;
    for (const auto& point : curve) {
        if (!std::isfinite(point.time) || !std::isfinite(point.value) ||
            point.time < 0.0f || point.time > 1.0f ||
            point.time <= previous || point.value < minimum ||
            point.value > maximum)
            return false;
        previous = point.time;
    }
    return true;
}

bool ValidColorCurve(const std::vector<ParticleColorPoint>& curve) {
    if (curve.size() > 8) return false;
    float previous = -1.0f;
    for (const auto& point : curve) {
        if (!std::isfinite(point.time) || point.time < 0.0f ||
            point.time > 1.0f || point.time <= previous)
            return false;
        previous = point.time;
    }
    return true;
}

float SampleRange(const ParticleRange& range, const std::uint32_t seed,
                  const std::uint64_t id, const std::uint32_t channel) {
    return range.minimum +
           (range.maximum - range.minimum) * Unit(seed, id, channel);
}

float SampleCurve(const std::vector<ParticleCurvePoint>& curve,
                  const float time, const float fallback) {
    if (curve.empty()) return fallback;
    if (time <= curve.front().time) return curve.front().value;
    for (std::size_t index = 1; index < curve.size(); ++index) {
        if (time > curve[index].time) continue;
        const float span = curve[index].time - curve[index - 1].time;
        const float local = span > 0.0f
                                ? (time - curve[index - 1].time) / span
                                : 0.0f;
        return curve[index - 1].value +
               (curve[index].value - curve[index - 1].value) * local;
    }
    return curve.back().value;
}

Color SampleColor(const std::vector<ParticleColorPoint>& curve,
                  const float time, const Color fallback) {
    if (curve.empty()) return fallback;
    if (time <= curve.front().time) return curve.front().value;
    for (std::size_t index = 1; index < curve.size(); ++index) {
        if (time > curve[index].time) continue;
        const float span = curve[index].time - curve[index - 1].time;
        const float local = span > 0.0f
                                ? (time - curve[index - 1].time) / span
                                : 0.0f;
        const auto channel = [local](const std::uint8_t from,
                                     const std::uint8_t to) {
            return static_cast<std::uint8_t>(std::clamp(
                std::lround(static_cast<float>(from) +
                            (static_cast<float>(to) - from) * local),
                0l, 255l));
        };
        return {channel(curve[index - 1].value.r, curve[index].value.r),
                channel(curve[index - 1].value.g, curve[index].value.g),
                channel(curve[index - 1].value.b, curve[index].value.b),
                channel(curve[index - 1].value.a, curve[index].value.a)};
    }
    return curve.back().value;
}

}  // namespace

std::string_view ParticlePresetName(const ParticlePreset preset) {
    switch (preset) {
        case ParticlePreset::Rain: return "rain";
        case ParticlePreset::Snow: return "snow";
        case ParticlePreset::Sakura: return "sakura";
        case ParticlePreset::Dust: return "dust";
    }
    return "rain";
}

std::optional<ParticlePreset> ParticlePresetFromName(
    const std::string_view name) {
    if (name == "rain") return ParticlePreset::Rain;
    if (name == "snow") return ParticlePreset::Snow;
    if (name == "sakura") return ParticlePreset::Sakura;
    if (name == "dust" || name == "light" || name == "motes")
        return ParticlePreset::Dust;
    return std::nullopt;
}

std::string_view ParticleSpawnShapeName(const ParticleSpawnShape shape) {
    switch (shape) {
        case ParticleSpawnShape::Point: return "point";
        case ParticleSpawnShape::Box: return "box";
        case ParticleSpawnShape::Line: return "line";
        case ParticleSpawnShape::Ellipse: return "ellipse";
    }
    return "box";
}

std::optional<ParticleSpawnShape> ParticleSpawnShapeFromName(
    const std::string_view name) {
    if (name == "point") return ParticleSpawnShape::Point;
    if (name == "box") return ParticleSpawnShape::Box;
    if (name == "line") return ParticleSpawnShape::Line;
    if (name == "ellipse") return ParticleSpawnShape::Ellipse;
    return std::nullopt;
}

bool ParticleSystem::Valid(const ParticleEmitterSpec& spec) {
    return spec.seed != 0 && std::isfinite(spec.rate) && spec.rate > 0.0f &&
           spec.rate <= 10'000.0f && spec.maxParticles > 0 &&
           spec.maxParticles <= 32'768 && std::isfinite(spec.opacity) &&
           spec.opacity >= 0.0f && spec.opacity <= 1.0f &&
           std::isfinite(spec.wind) && std::abs(spec.wind) <= 20.0f &&
           std::isfinite(spec.speed) && spec.speed > 0.0f &&
           spec.speed <= 20.0f && std::isfinite(spec.size) &&
           spec.size > 0.0f && spec.size <= 20.0f &&
           SafeTexturePath(spec.texture) && spec.atlasColumns > 0 &&
           spec.atlasColumns <= 64 && spec.atlasRows > 0 &&
           spec.atlasRows <= 64 && spec.atlasFirstFrame <
               spec.atlasColumns * spec.atlasRows &&
           spec.atlasFrameCount > 0 && spec.atlasFrameCount <=
               spec.atlasColumns * spec.atlasRows - spec.atlasFirstFrame &&
           ValidRange(spec.positionX, -16.0f, 16.0f) &&
           ValidRange(spec.positionY, -16.0f, 16.0f) &&
           ValidRange(spec.velocityX, -100'000.0f, 100'000.0f) &&
           ValidRange(spec.velocityY, -100'000.0f, 100'000.0f) &&
           ValidRange(spec.accelerationX, -100'000.0f, 100'000.0f) &&
           ValidRange(spec.accelerationY, -100'000.0f, 100'000.0f) &&
           ValidRange(spec.lifetime, 0.01f, 3'600.0f) &&
           ValidRange(spec.rotation, -1'000'000.0f, 1'000'000.0f) &&
           ValidRange(spec.angularVelocity, -100'000.0f, 100'000.0f) &&
           ValidRange(spec.scale, 0.001f, 1'000.0f) &&
           ValidRange(spec.initialOpacity, 0.0f, 1.0f) &&
           ValidCurve(spec.scaleOverLifetime, 0.0f, 1'000.0f) &&
           ValidCurve(spec.opacityOverLifetime, 0.0f, 1.0f) &&
           ValidColorCurve(spec.colorOverLifetime) &&
           std::isfinite(spec.gravity) && std::abs(spec.gravity) <= 100'000.0f &&
           std::isfinite(spec.variation) && spec.variation >= 0.0f &&
           spec.variation <= 1.0f && spec.burst <= spec.maxParticles &&
           std::isfinite(spec.duration) && spec.duration >= 0.0f &&
           spec.duration <= 86'400.0f;
}

bool ParticleSystem::Set(std::string name, const ParticleEmitterSpec& spec) {
    if (name.empty() || name.size() > 128 || !Valid(spec)) return false;
    auto found = m_emitters.find(name);
    if (found == m_emitters.end()) {
        m_emitters.emplace(name, ParticleEmitterState{name, spec, 0, 0.0});
    } else {
        // Changing an emitter is an authored reset, not a nondeterministic
        // mutation of the existing particle stream.
        found->second.spec = spec;
        found->second.ticks = 0;
        found->second.tickRemainder = 0.0;
    }
    return true;
}

void ParticleSystem::Clear(const std::string_view name) {
    m_emitters.erase(std::string(name));
}

void ParticleSystem::ClearAll() { m_emitters.clear(); }

void ParticleSystem::Update(const float deltaSeconds) {
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f) return;
    const double ticks =
        std::clamp(static_cast<double>(deltaSeconds), 0.0, 0.25) *
        kTicksPerSecond;
    for (auto& [_, emitter] : m_emitters) {
        emitter.tickRemainder += ticks;
        const auto whole =
            static_cast<std::uint64_t>(std::floor(emitter.tickRemainder));
        if (whole > kMaxEmitterTicks - emitter.ticks) {
            emitter.ticks = whole - (kMaxEmitterTicks - emitter.ticks) - 1;
        } else {
            emitter.ticks += whole;
        }
        emitter.tickRemainder -= static_cast<double>(whole);
    }
}

std::vector<ParticleSample> ParticleSystem::Samples(
    const std::string_view name, const int width, const int height) const {
    const auto found = m_emitters.find(std::string(name));
    if (found == m_emitters.end() || width <= 0 || height <= 0) return {};
    const auto& emitter = found->second;
    const auto& spec = emitter.spec;
    const double now = static_cast<double>(emitter.ticks) +
                       emitter.tickRemainder;
    const double seconds = now / kTicksPerSecond;
    const double interval = 1.0 / spec.rate;
    const std::uint64_t regularPerCycle = spec.duration > 0.0f
        ? static_cast<std::uint64_t>(
              std::floor(static_cast<double>(spec.duration) * spec.rate))
        : 0;
    const std::uint64_t eventsPerCycle =
        static_cast<std::uint64_t>(spec.burst) + regularPerCycle;
    std::uint64_t totalSpawned = 0;
    if (spec.advanced && !spec.loop && spec.duration <= 0.0f) {
        totalSpawned = spec.burst;
    } else if (spec.advanced && spec.duration > 0.0f) {
        const auto completedCycles = spec.loop
            ? static_cast<std::uint64_t>(std::floor(seconds / spec.duration))
            : 0u;
        const double cycleTime = spec.loop
            ? seconds - static_cast<double>(completedCycles) * spec.duration
            : std::min<double>(seconds, spec.duration);
        const std::uint64_t currentRegular = std::min<std::uint64_t>(
            regularPerCycle,
            static_cast<std::uint64_t>(std::floor(cycleTime * spec.rate)));
        totalSpawned = completedCycles * eventsPerCycle + spec.burst +
                       currentRegular;
    } else {
        totalSpawned = (spec.advanced ? spec.burst : 0u) +
                       static_cast<std::uint64_t>(
                           std::floor(seconds * spec.rate));
    }
    const std::uint64_t first =
        totalSpawned > spec.maxParticles ? totalSpawned - spec.maxParticles : 0;
    std::vector<ParticleSample> samples;
    samples.reserve(static_cast<std::size_t>(totalSpawned - first));
    for (std::uint64_t id = first; id < totalSpawned; ++id) {
        double born = 0.0;
        if (spec.advanced && spec.duration > 0.0f && eventsPerCycle > 0) {
            const std::uint64_t cycle = id / eventsPerCycle;
            const std::uint64_t local = id % eventsPerCycle;
            born = static_cast<double>(cycle) * spec.duration;
            if (local >= spec.burst)
                born += static_cast<double>(local - spec.burst + 1u) * interval;
        } else if (spec.advanced) {
            born = id < spec.burst
                       ? 0.0
                       : static_cast<double>(id - spec.burst + 1u) * interval;
        } else {
            born = static_cast<double>(id + 1u) * interval;
        }
        const float age = static_cast<float>(seconds - born);
        const float variation = Unit(spec.seed, id, 3);
        float lifetime = spec.advanced
                             ? SampleRange(spec.lifetime, spec.seed, id, 3)
                             : 2.0f;
        if (!spec.advanced) {
            switch (spec.preset) {
                case ParticlePreset::Rain: lifetime = 1.1f + variation * 1.1f; break;
                case ParticlePreset::Snow: lifetime = 5.0f + variation * 3.0f; break;
                case ParticlePreset::Sakura: lifetime = 5.5f + variation * 3.5f; break;
                case ParticlePreset::Dust: lifetime = 7.0f + variation * 4.0f; break;
            }
        }
        if (age < 0.0f || age > lifetime) continue;
        const float u = Unit(spec.seed, id, 0);
        const float phase = Unit(spec.seed, id, 1) * 6.2831853f;
        float particleScale = (0.6f + Unit(spec.seed, id, 2) * 0.8f) * spec.size;
        float x = u * static_cast<float>(width);
        float y = 0.0f;
        Vec2 extent{particleScale * 3.0f, particleScale * 3.0f};
        float rotation = 0.0f;
        if (spec.advanced) {
            float normalizedX = SampleRange(spec.positionX, spec.seed, id, 4);
            float normalizedY = SampleRange(spec.positionY, spec.seed, id, 5);
            if (spec.spawnShape == ParticleSpawnShape::Line) {
                const float point = Unit(spec.seed, id, 4);
                normalizedX = spec.positionX.minimum +
                    (spec.positionX.maximum - spec.positionX.minimum) * point;
                normalizedY = spec.positionY.minimum +
                    (spec.positionY.maximum - spec.positionY.minimum) * point;
            } else if (spec.spawnShape == ParticleSpawnShape::Ellipse) {
                const float angle = Unit(spec.seed, id, 4) * 6.2831853f;
                const float radius = std::sqrt(Unit(spec.seed, id, 5));
                normalizedX = (spec.positionX.minimum + spec.positionX.maximum) * 0.5f +
                    std::cos(angle) * radius *
                    (spec.positionX.maximum - spec.positionX.minimum) * 0.5f;
                normalizedY = (spec.positionY.minimum + spec.positionY.maximum) * 0.5f +
                    std::sin(angle) * radius *
                    (spec.positionY.maximum - spec.positionY.minimum) * 0.5f;
            } else if (spec.spawnShape == ParticleSpawnShape::Point) {
                normalizedX = spec.positionX.minimum;
                normalizedY = spec.positionY.minimum;
            }
            const float randomized = 1.0f +
                (Unit(spec.seed, id, 6) * 2.0f - 1.0f) * spec.variation;
            const float velocityX =
                SampleRange(spec.velocityX, spec.seed, id, 7) * randomized;
            const float velocityY =
                SampleRange(spec.velocityY, spec.seed, id, 8) * randomized;
            const float accelerationX =
                SampleRange(spec.accelerationX, spec.seed, id, 9) + spec.wind;
            const float accelerationY =
                SampleRange(spec.accelerationY, spec.seed, id, 10) + spec.gravity;
            x = normalizedX * static_cast<float>(width) + velocityX * age +
                accelerationX * age * age * 0.5f;
            y = normalizedY * static_cast<float>(height) + velocityY * age +
                accelerationY * age * age * 0.5f;
            particleScale = SampleRange(spec.scale, spec.seed, id, 11) *
                SampleCurve(spec.scaleOverLifetime,
                            std::clamp(age / lifetime, 0.0f, 1.0f), 1.0f) *
                spec.size * randomized;
            extent = {std::max(0.01f, 16.0f * particleScale),
                      std::max(0.01f, 16.0f * particleScale)};
            rotation = SampleRange(spec.rotation, spec.seed, id, 12) +
                SampleRange(spec.angularVelocity, spec.seed, id, 13) * age;
        } else {
            switch (spec.preset) {
                case ParticlePreset::Rain:
                    x += age * (spec.wind * 28.0f + 18.0f);
                    y = age * 620.0f * spec.speed - 28.0f;
                    extent = {std::max(1.0f, particleScale), 11.0f * particleScale};
                    break;
                case ParticlePreset::Snow:
                    x += std::sin(age * 1.8f + phase) * 28.0f + age * spec.wind * 12.0f;
                    y = age * 72.0f * spec.speed - 10.0f;
                    break;
                case ParticlePreset::Sakura:
                    x += std::sin(age * 2.3f + phase) * 44.0f +
                         age * (22.0f + spec.wind * 18.0f);
                    y = age * 86.0f * spec.speed - 12.0f;
                    extent = {5.0f * particleScale, 3.0f * particleScale};
                    rotation = age * 90.0f + phase * 57.2958f;
                    break;
                case ParticlePreset::Dust:
                    x += std::sin(age * 0.8f + phase) * 18.0f + age * spec.wind * 4.0f;
                    y = static_cast<float>(height) * Unit(spec.seed, id, 4) +
                        std::sin(age * 0.7f + phase) * 14.0f;
                    extent = {2.2f * particleScale, 2.2f * particleScale};
                    break;
            }
        }
        const float normalized = std::clamp(age / lifetime, 0.0f, 1.0f);
        const float fade =
            std::min(1.0f, age * 5.0f) * std::min(1.0f, (1.0f - normalized) * 5.0f);
        const float opacityCurve = spec.advanced
            ? SampleCurve(spec.opacityOverLifetime, normalized, fade)
            : fade;
        const float initialOpacity = spec.advanced
            ? SampleRange(spec.initialOpacity, spec.seed, id, 14)
            : 1.0f;
        Color color = SampleColor(spec.colorOverLifetime, normalized,
                                  PresetColor(spec.preset, 1.0f));
        color.a = static_cast<std::uint8_t>(std::clamp(
            static_cast<float>(color.a) * spec.opacity * initialOpacity *
                opacityCurve,
            0.0f, 255.0f));
        Rect source{};
        if (!spec.texture.empty()) {
            const std::uint32_t frame = spec.atlasFirstFrame +
                static_cast<std::uint32_t>(id % spec.atlasFrameCount);
            const std::uint32_t column = frame % spec.atlasColumns;
            const std::uint32_t row = frame / spec.atlasColumns;
            source = {static_cast<float>(column) / spec.atlasColumns,
                      static_cast<float>(row) / spec.atlasRows,
                      1.0f / spec.atlasColumns, 1.0f / spec.atlasRows};
        }
        ParticleSample sample;
        sample.id = id;
        sample.position = {x, y};
        sample.extent = extent;
        sample.color = color;
        sample.rotation = rotation;
        sample.source = source;
        sample.texture = spec.texture;
        samples.push_back(std::move(sample));
    }
    return samples;
}

void ParticleSystem::Render(const bool front) const {
    if (!front) m_lastRenderStats = {};
    int width = 0;
    int height = 0;
    m_renderer.GetLogicalSize(width, height);
    std::vector<const ParticleEmitterState*> ordered;
    for (const auto& [_, emitter] : m_emitters) {
        if ((front && emitter.spec.z >= 0) ||
            (!front && emitter.spec.z < 0))
            ordered.push_back(&emitter);
    }
    std::ranges::sort(ordered, [](const auto* left, const auto* right) {
        if (left->spec.z != right->spec.z)
            return left->spec.z < right->spec.z;
        return left->name < right->name;
    });
    for (const auto* emitter : ordered) {
        const auto particles = Samples(emitter->name, width, height);
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
        const auto before = m_renderer.GeometryBatchCount();
        m_renderer.DrawSpriteBatch(emitter->spec.texture, batch);
        if (m_renderer.GeometryBatchCount() != before) {
            ++m_lastRenderStats.batches;
            m_lastRenderStats.particles += batch.size();
        }
    }
}

std::vector<ParticleEmitterState> ParticleSystem::Capture() const {
    std::vector<ParticleEmitterState> state;
    state.reserve(m_emitters.size());
    for (const auto& [_, emitter] : m_emitters) state.push_back(emitter);
    std::ranges::sort(state, {}, &ParticleEmitterState::name);
    return state;
}

Status ParticleSystem::Restore(
    const std::vector<ParticleEmitterState>& state) {
    std::unordered_map<std::string, ParticleEmitterState> candidate;
    for (const auto& emitter : state) {
        if (emitter.name.empty() || emitter.name.size() > 128 ||
            !Valid(emitter.spec) || !std::isfinite(emitter.tickRemainder) ||
            emitter.ticks > kMaxEmitterTicks ||
            emitter.tickRemainder < 0.0 || emitter.tickRemainder >= 1.0 ||
            !candidate.emplace(emitter.name, emitter).second)
            return Status::Fail(diag::Diagnostic{
                .severity = diag::Severity::Error,
                .code = "PXSTAGE7520",
                .category = "Runtime.Stage",
                .message = "Saved particle emitter state is invalid",
                .details = emitter.name});
    }
    m_emitters = std::move(candidate);
    return Status::Ok();
}

}  // namespace px::vn
