#include "Engine/Graphics/Compositor2D.h"

#include "Engine/Graphics/BuiltInShaders.h"
#include "Engine/IO/Crypto.h"
#include "Engine/IO/VFS.h"
#include "Engine/Support/Logger.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <ranges>

namespace px::graphics {

Compositor2D::Compositor2D(SDL_Renderer* renderer, const bool enabled)
    : m_renderer(renderer), m_required(enabled) {
    m_enabled = enabled && CreateGpuState();
}

Compositor2D::~Compositor2D() {
    DestroyTarget();
    DestroyGpuState();
}

bool Compositor2D::CreateGpuState() {
#if defined(__EMSCRIPTEN__)
    return false;
#else
    SDL_GPUDevice* device = SDL_GetGPURendererDevice(m_renderer);
    if (!device) {
        PX_LOG_ERROR("GPU compositor requires SDL's GPU renderer: {}",
                     SDL_GetError());
        return false;
    }
    const auto bytecode = SelectBuiltInCompositorShader(
        SDL_GetGPUShaderFormats(device));
    if (!bytecode || !bytecode->data || bytecode->size == 0) {
        PX_LOG_ERROR("GPU compositor has no offline shader artifact accepted by the device");
        return false;
    }

    SDL_GPUShaderCreateInfo shaderInfo{};
    shaderInfo.code = bytecode->data;
    shaderInfo.code_size = bytecode->size;
    shaderInfo.entrypoint = bytecode->entrypoint;
    shaderInfo.format = bytecode->format;
    shaderInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    shaderInfo.num_samplers = 1;
    shaderInfo.num_uniform_buffers = 1;
    m_shader = SDL_CreateGPUShader(device, &shaderInfo);
    if (!m_shader) {
        PX_LOG_ERROR("Offline compositor shader was rejected: {}", SDL_GetError());
        return false;
    }

    SDL_GPURenderStateCreateInfo stateInfo{};
    stateInfo.fragment_shader = m_shader;
    m_renderState = SDL_CreateGPURenderState(m_renderer, &stateInfo);
    if (!m_renderState) {
        PX_LOG_ERROR("GPU compositor render state creation failed: {}", SDL_GetError());
        SDL_ReleaseGPUShader(device, m_shader);
        m_shader = nullptr;
        return false;
    }
    return true;
#endif
}

void Compositor2D::DestroyGpuState() {
#if !defined(__EMSCRIPTEN__)
    SDL_GPUDevice* device = SDL_GetGPURendererDevice(m_renderer);
    for (auto& [_, effect] : m_customEffects) {
        if (effect.state) SDL_DestroyGPURenderState(effect.state);
        if (effect.shader && device) SDL_ReleaseGPUShader(device, effect.shader);
    }
    m_customEffects.clear();
    if (m_renderState) SDL_DestroyGPURenderState(m_renderState);
    m_renderState = nullptr;
    if (m_shader) {
        if (device) SDL_ReleaseGPUShader(device, m_shader);
    }
    m_shader = nullptr;
#endif
}

bool Compositor2D::LoadCustomEffects(
    const std::vector<CustomEffectDescriptor>& effects, const io::VFS& vfs) {
#if defined(__EMSCRIPTEN__)
    (void)vfs;
    return effects.empty();
#else
    if (effects.empty()) return true;
    if (!m_enabled) return false;
    SDL_GPUDevice* device = SDL_GetGPURendererDevice(m_renderer);
    if (!device) return false;
    const SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(device);
    std::unordered_map<std::string, CustomEffectState> candidate;
    const auto rejectCandidate = [&candidate, device] {
        for (auto& [_, built] : candidate) {
            if (built.state) SDL_DestroyGPURenderState(built.state);
            if (built.shader) SDL_ReleaseGPUShader(device, built.shader);
        }
        candidate.clear();
        return false;
    };
    for (const auto& descriptor : effects) {
        if (descriptor.id.empty() || descriptor.targetLayer != "stage" ||
            descriptor.samplerCount != 1 ||
            descriptor.uniformBufferCount != 1 ||
            descriptor.uniforms.size() > 8 || descriptor.artifacts.size() != 3 ||
            candidate.contains(descriptor.id))
            return rejectCandidate();
        const CustomEffectArtifactDescriptor* selected = nullptr;
        SDL_GPUShaderFormat selectedFormat = SDL_GPU_SHADERFORMAT_INVALID;
        const auto select = [&](const std::string_view name,
                                const SDL_GPUShaderFormat format) {
            if (selected || (supported & format) == 0) return;
            const auto found = std::ranges::find(
                descriptor.artifacts, name,
                &CustomEffectArtifactDescriptor::format);
            if (found != descriptor.artifacts.end()) {
                selected = &*found;
                selectedFormat = format;
            }
        };
        select("spirv", SDL_GPU_SHADERFORMAT_SPIRV);
        select("dxil", SDL_GPU_SHADERFORMAT_DXIL);
        select("msl", SDL_GPU_SHADERFORMAT_MSL);
        if (!selected) return rejectCandidate();
        const auto bytes = vfs.Read(selected->asset);
        if (!bytes || bytes->empty() || bytes->size() > 4 * 1024 * 1024)
            return rejectCandidate();
        const std::string_view artifactBytes(
            reinterpret_cast<const char*>(bytes->data()), bytes->size());
        if (crypto::Sha256Hex(artifactBytes) != selected->fingerprint) {
            PX_LOG_ERROR("Custom effect '{}' artifact fingerprint mismatch",
                         descriptor.id);
            return rejectCandidate();
        }
        SDL_GPUShaderCreateInfo shaderInfo{};
        shaderInfo.code = bytes->data();
        shaderInfo.code_size = bytes->size();
        shaderInfo.entrypoint = selectedFormat == SDL_GPU_SHADERFORMAT_MSL
                                    ? "main0"
                                    : "main";
        shaderInfo.format = selectedFormat;
        shaderInfo.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        shaderInfo.num_samplers = descriptor.samplerCount;
        shaderInfo.num_uniform_buffers = descriptor.uniformBufferCount;
        SDL_GPUShader* shader = SDL_CreateGPUShader(device, &shaderInfo);
        if (!shader) {
            PX_LOG_ERROR("Custom effect '{}' shader was rejected: {}",
                         descriptor.id, SDL_GetError());
            return rejectCandidate();
        }
        SDL_GPURenderStateCreateInfo stateInfo{};
        stateInfo.fragment_shader = shader;
        SDL_GPURenderState* state = SDL_CreateGPURenderState(m_renderer, &stateInfo);
        if (!state) {
            SDL_ReleaseGPUShader(device, shader);
            return rejectCandidate();
        }
        candidate.emplace(descriptor.id,
                          CustomEffectState{descriptor, shader, state});
    }
    for (auto& [_, effect] : m_customEffects) {
        if (effect.state) SDL_DestroyGPURenderState(effect.state);
        if (effect.shader) SDL_ReleaseGPUShader(device, effect.shader);
    }
    m_customEffects = std::move(candidate);
    return true;
#endif
}

bool Compositor2D::HasCustomEffect(const std::string_view id) const {
    return m_customEffects.contains(std::string(id));
}

std::vector<std::string> Compositor2D::CustomEffectIds() const {
    std::vector<std::string> result;
    result.reserve(m_customEffects.size());
    for (const auto& [id, _] : m_customEffects) result.push_back(id);
    std::ranges::sort(result);
    return result;
}

std::optional<std::array<std::array<float, 4>, 8>>
Compositor2D::CustomEffectDefaults(const std::string_view id) const {
    const auto found = m_customEffects.find(std::string(id));
    if (found == m_customEffects.end()) return std::nullopt;
    std::array<std::array<float, 4>, 8> result{};
    for (const auto& uniform : found->second.descriptor.uniforms) {
        if (uniform.slot >= result.size()) return std::nullopt;
        result[uniform.slot] = uniform.defaultValue;
    }
    return result;
}

std::optional<std::array<std::array<float, 4>, 8>>
Compositor2D::ResolveCustomEffectParameters(
    const std::string_view id,
    const CustomEffectNamedParameters& parameters) const {
    const auto found = m_customEffects.find(std::string(id));
    if (found == m_customEffects.end() ||
        parameters.size() > found->second.descriptor.uniforms.size())
        return std::nullopt;
    auto result = CustomEffectDefaults(id);
    if (!result) return std::nullopt;
    for (const auto& [name, values] : parameters) {
        const auto uniform = std::ranges::find(
            found->second.descriptor.uniforms, name,
            &CustomEffectUniformDescriptor::name);
        if (uniform == found->second.descriptor.uniforms.end() ||
            uniform->slot >= result->size())
            return std::nullopt;
        const std::size_t components = uniform->type == "number" ? 1u
                                     : uniform->type == "vec2" ? 2u
                                     : uniform->type == "color" ? 4u : 0u;
        if (components == 0 || values.size() != components) return std::nullopt;
        std::array<float, 4> slot{};
        for (std::size_t component = 0; component < components; ++component) {
            const float value = values[component];
            if (!std::isfinite(value) || value < uniform->minimum ||
                value > uniform->maximum ||
                (uniform->type == "color" && (value < 0.0f || value > 1.0f)))
                return std::nullopt;
            slot[component] = value;
        }
        (*result)[uniform->slot] = slot;
    }
    return result;
}

void Compositor2D::SetLogicalSize(const int width, const int height) {
    if (m_width == width && m_height == height) return;
    m_width = width;
    m_height = height;
    DestroyTarget();
}

bool Compositor2D::EnsureTarget() {
    if (!m_enabled || !m_renderer || m_width <= 0 || m_height <= 0) return false;
    if (m_stage) return true;
    m_stage = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA8888,
                                SDL_TEXTUREACCESS_TARGET, m_width, m_height);
    if (!m_stage) {
        m_enabled = false;
        return false;
    }
    SDL_SetTextureBlendMode(m_stage, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(m_stage, SDL_SCALEMODE_LINEAR);
    m_intermediate = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA8888,
                                       SDL_TEXTUREACCESS_TARGET, m_width, m_height);
    if (!m_intermediate) {
        SDL_DestroyTexture(m_stage);
        m_stage = nullptr;
        m_enabled = false;
        return false;
    }
    SDL_SetTextureBlendMode(m_intermediate, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(m_intermediate, SDL_SCALEMODE_LINEAR);
    return true;
}

void Compositor2D::DestroyTarget() {
    if (m_stage) SDL_DestroyTexture(m_stage);
    m_stage = nullptr;
    if (m_intermediate) SDL_DestroyTexture(m_intermediate);
    m_intermediate = nullptr;
    m_recording = false;
    m_previousTarget = nullptr;
}

bool Compositor2D::BeginStage() {
    if (!EnsureTarget() || m_recording) return false;
    m_previousTarget = SDL_GetRenderTarget(m_renderer);
    if (!SDL_SetRenderTarget(m_renderer, m_stage)) return false;
    SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 0);
    SDL_RenderClear(m_renderer);
    m_recording = true;
    return true;
}

void Compositor2D::EndStage(const StagePostEffects& effects) {
    if (!m_recording) return;
    SDL_SetRenderTarget(m_renderer, m_previousTarget);
    m_recording = false;
    struct alignas(16) CompositorUniforms {
        float texelSize[2];
        float blur;
        float vignette;
        float colorGrade;
        float randomSeed;
        float padding[2];
    } uniforms{{1.0f / static_cast<float>(m_width),
                1.0f / static_cast<float>(m_height)},
               std::clamp(effects.blur, 0.0f, 1.0f),
               std::clamp(effects.vignette, 0.0f, 1.0f),
               std::clamp(effects.colorGrade, 0.0f, 1.0f),
               effects.randomSeed,
               {0.0f, 0.0f}};
    const auto custom = m_customEffects.find(effects.customEffect);
    if (custom == m_customEffects.end()) {
        (void)ApplyRenderState(m_renderState, m_stage, m_previousTarget,
                               &uniforms, sizeof(uniforms));
        return;
    }
    const bool builtInActive = uniforms.blur > 0.0f ||
                               uniforms.vignette > 0.0f ||
                               uniforms.colorGrade > 0.0f;
    SDL_Texture* customSource = m_stage;
    if (builtInActive) {
        if (!ApplyRenderState(m_renderState, m_stage, m_intermediate,
                              &uniforms, sizeof(uniforms)))
            return;
        customSource = m_intermediate;
    }
    struct alignas(16) CustomUniforms {
        float texelSize[2];
        float progress;
        float randomSeed;
        float parameters[8][4];
    } customUniforms{};
    customUniforms.texelSize[0] = 1.0f / static_cast<float>(m_width);
    customUniforms.texelSize[1] = 1.0f / static_cast<float>(m_height);
    customUniforms.progress = std::clamp(effects.customProgress, 0.0f, 1.0f);
    customUniforms.randomSeed = effects.randomSeed;
    for (std::size_t slot = 0; slot < effects.customParameters.size(); ++slot)
        std::ranges::copy(effects.customParameters[slot],
                          customUniforms.parameters[slot]);
    (void)ApplyRenderState(custom->second.state, customSource,
                           m_previousTarget, &customUniforms,
                           sizeof(customUniforms));
}

bool Compositor2D::ApplyRenderState(SDL_GPURenderState* state,
                                    SDL_Texture* source, SDL_Texture* target,
                                    const void* uniforms,
                                    const std::uint32_t uniformBytes) {
    if (!state || !source || !uniforms || uniformBytes == 0) return false;
    const auto fail = [this] {
        (void)SDL_SetGPURenderState(m_renderer, nullptr);
        (void)SDL_SetRenderTarget(m_renderer, m_previousTarget);
        return false;
    };
    if (!SDL_SetRenderTarget(m_renderer, target)) return fail();
    if (target == m_intermediate) {
        SDL_SetRenderDrawColor(m_renderer, 0, 0, 0, 0);
        SDL_RenderClear(m_renderer);
    }
    if (!SDL_SetGPURenderStateFragmentUniforms(
            state, 0, uniforms, uniformBytes)) {
        PX_LOG_ERROR("GPU effect uniform update failed: {}", SDL_GetError());
        return fail();
    }
    if (!SDL_SetGPURenderState(m_renderer, state)) {
        PX_LOG_ERROR("GPU effect activation failed: {}", SDL_GetError());
        return fail();
    }
    const SDL_FRect destination{0, 0, static_cast<float>(m_width),
                                static_cast<float>(m_height)};
    const bool rendered = SDL_RenderTexture(m_renderer, source, nullptr,
                                            &destination);
    (void)SDL_SetGPURenderState(m_renderer, nullptr);
    if (!rendered) {
        PX_LOG_ERROR("GPU effect draw failed: {}", SDL_GetError());
        return fail();
    }
    return true;
}

}  // namespace px::graphics
