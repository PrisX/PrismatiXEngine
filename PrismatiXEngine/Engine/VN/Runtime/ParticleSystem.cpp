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

bool ParticleSystem::Valid(const ParticleEmitterSpec& spec) {
    return spec.seed != 0 && std::isfinite(spec.rate) && spec.rate > 0.0f &&
           spec.rate <= 2'000.0f && spec.maxParticles > 0 &&
           spec.maxParticles <= 4'096 && std::isfinite(spec.opacity) &&
           spec.opacity >= 0.0f && spec.opacity <= 1.0f &&
           std::isfinite(spec.wind) && std::abs(spec.wind) <= 20.0f &&
           std::isfinite(spec.speed) && spec.speed > 0.0f &&
           spec.speed <= 20.0f && std::isfinite(spec.size) &&
           spec.size > 0.0f && spec.size <= 20.0f;
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
    const double interval = kTicksPerSecond / spec.rate;
    const std::uint64_t totalSpawned =
        static_cast<std::uint64_t>(std::floor(now / interval));
    const std::uint64_t first =
        totalSpawned > spec.maxParticles ? totalSpawned - spec.maxParticles : 0;
    std::vector<ParticleSample> samples;
    samples.reserve(static_cast<std::size_t>(totalSpawned - first));
    for (std::uint64_t id = first; id < totalSpawned; ++id) {
        const double born = static_cast<double>(id + 1) * interval;
        const float age = static_cast<float>((now - born) / kTicksPerSecond);
        const float variation = Unit(spec.seed, id, 3);
        float lifetime = 2.0f;
        switch (spec.preset) {
            case ParticlePreset::Rain: lifetime = 1.1f + variation * 1.1f; break;
            case ParticlePreset::Snow: lifetime = 5.0f + variation * 3.0f; break;
            case ParticlePreset::Sakura: lifetime = 5.5f + variation * 3.5f; break;
            case ParticlePreset::Dust: lifetime = 7.0f + variation * 4.0f; break;
        }
        if (age < 0.0f || age > lifetime) continue;
        const float u = Unit(spec.seed, id, 0);
        const float phase = Unit(spec.seed, id, 1) * 6.2831853f;
        const float scale = (0.6f + Unit(spec.seed, id, 2) * 0.8f) * spec.size;
        float x = u * static_cast<float>(width);
        float y = 0.0f;
        Vec2 extent{scale * 3.0f, scale * 3.0f};
        switch (spec.preset) {
            case ParticlePreset::Rain:
                x += age * (spec.wind * 28.0f + 18.0f);
                y = age * 620.0f * spec.speed - 28.0f;
                extent = {std::max(1.0f, scale), 11.0f * scale};
                break;
            case ParticlePreset::Snow:
                x += std::sin(age * 1.8f + phase) * 28.0f +
                     age * spec.wind * 12.0f;
                y = age * 72.0f * spec.speed - 10.0f;
                break;
            case ParticlePreset::Sakura:
                x += std::sin(age * 2.3f + phase) * 44.0f +
                     age * (22.0f + spec.wind * 18.0f);
                y = age * 86.0f * spec.speed - 12.0f;
                extent = {5.0f * scale, 3.0f * scale};
                break;
            case ParticlePreset::Dust:
                x += std::sin(age * 0.8f + phase) * 18.0f +
                     age * spec.wind * 4.0f;
                y = static_cast<float>(height) * Unit(spec.seed, id, 4) +
                    std::sin(age * 0.7f + phase) * 14.0f;
                extent = {2.2f * scale, 2.2f * scale};
                break;
        }
        const float normalized = std::clamp(age / lifetime, 0.0f, 1.0f);
        const float fade =
            std::min(1.0f, age * 5.0f) * std::min(1.0f, (1.0f - normalized) * 5.0f);
        samples.push_back({id, {x, y}, extent,
                           PresetColor(spec.preset, spec.opacity * fade)});
    }
    return samples;
}

void ParticleSystem::Render(const bool front) const {
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
        for (const auto& particle : Samples(emitter->name, width, height))
            m_renderer.DrawRect(
                {particle.position.x, particle.position.y,
                 particle.extent.x, particle.extent.y},
                particle.color);
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
