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

namespace {

struct alignas(16) CustomUniformsV2 {
    float texelSize[2];
    float progress;
    float randomSeed;
    float parameters[8][4];
};

struct alignas(16) CustomUniformsV3 : CustomUniformsV2 {
    float viewport[4];
};

[[maybe_unused]] bool SetCustomUniforms(
    SDL_Renderer* renderer, SDL_GPURenderState* state,
    const CustomEffectDescriptor& descriptor, const int width, const int height,
    const float progress, const float randomSeed,
    const std::array<std::array<float, 4>, 8>& parameters,
    const std::array<float, 4>& viewport) {
    if (!renderer || !state || width <= 0 || height <= 0) return false;
    CustomUniformsV3 uniforms{};
    uniforms.texelSize[0] = 1.0f / static_cast<float>(width);
    uniforms.texelSize[1] = 1.0f / static_cast<float>(height);
    uniforms.progress = std::clamp(progress, 0.0f, 1.0f);
    uniforms.randomSeed = randomSeed;
    for (std::size_t slot = 0; slot < parameters.size(); ++slot)
        std::ranges::copy(parameters[slot], uniforms.parameters[slot]);
    std::ranges::copy(viewport, uniforms.viewport);
    const std::uint32_t bytes = descriptor.schemaRevision >= 3
                                    ? sizeof(CustomUniformsV3)
                                    : sizeof(CustomUniformsV2);
    return SDL_SetGPURenderStateFragmentUniforms(state, 0, &uniforms, bytes);
}

}  // namespace

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
        for (auto& [__, state] : effect.transitionStates)
            if (state) SDL_DestroyGPURenderState(state);
        if (effect.state) SDL_DestroyGPURenderState(effect.state);
        if (effect.transitionSampler && device)
            SDL_ReleaseGPUSampler(device, effect.transitionSampler);
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
    SDL_GPUDevice* device = SDL_GetGPURendererDevice(m_renderer);
    if (effects.empty()) {
        for (auto& [_, effect] : m_customEffects) {
            for (auto& [__, state] : effect.transitionStates)
                if (state) SDL_DestroyGPURenderState(state);
            if (effect.state) SDL_DestroyGPURenderState(effect.state);
            if (effect.transitionSampler && device)
                SDL_ReleaseGPUSampler(device, effect.transitionSampler);
            if (effect.shader && device)
                SDL_ReleaseGPUShader(device, effect.shader);
        }
        m_customEffects.clear();
        return true;
    }
    if (!m_enabled) return false;
    if (!device) return false;
    const SDL_GPUShaderFormat supported = SDL_GetGPUShaderFormats(device);
    std::unordered_map<std::string, CustomEffectState> candidate;
    const auto rejectCandidate = [&candidate, device] {
        for (auto& [_, built] : candidate) {
            for (auto& [__, state] : built.transitionStates)
                if (state) SDL_DestroyGPURenderState(state);
            if (built.state) SDL_DestroyGPURenderState(built.state);
            if (built.transitionSampler)
                SDL_ReleaseGPUSampler(device, built.transitionSampler);
            if (built.shader) SDL_ReleaseGPUShader(device, built.shader);
        }
        candidate.clear();
        return false;
    };
    for (const auto& descriptor : effects) {
        const bool validTarget = IsCustomEffectTarget(descriptor.targetLayer);
        const std::uint32_t expectedSamplers =
            descriptor.targetLayer == "transition" ? 2u : 1u;
        if (descriptor.id.empty() || !validTarget ||
            (descriptor.schemaRevision != 2 &&
             descriptor.schemaRevision != 3) ||
            (descriptor.schemaRevision == 2 &&
             descriptor.targetLayer != "stage") ||
            descriptor.samplerCount != expectedSamplers ||
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
        SDL_GPURenderState* state = nullptr;
        SDL_GPUSampler* transitionSampler = nullptr;
        if (descriptor.targetLayer == "transition") {
            SDL_GPUSamplerCreateInfo samplerInfo{};
            samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
            samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
            samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
            samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            transitionSampler = SDL_CreateGPUSampler(device, &samplerInfo);
            if (!transitionSampler) {
                SDL_ReleaseGPUShader(device, shader);
                return rejectCandidate();
            }
        } else {
            SDL_GPURenderStateCreateInfo stateInfo{};
            stateInfo.fragment_shader = shader;
            state = SDL_CreateGPURenderState(m_renderer, &stateInfo);
            if (!state) {
                SDL_ReleaseGPUShader(device, shader);
                return rejectCandidate();
            }
        }
        CustomEffectState built;
        built.descriptor = descriptor;
        built.shader = shader;
        built.state = state;
        built.transitionSampler = transitionSampler;
        candidate.emplace(descriptor.id, std::move(built));
    }
    for (auto& [_, effect] : m_customEffects) {
        for (auto& [__, state] : effect.transitionStates)
            if (state) SDL_DestroyGPURenderState(state);
        if (effect.state) SDL_DestroyGPURenderState(effect.state);
        if (effect.transitionSampler)
            SDL_ReleaseGPUSampler(device, effect.transitionSampler);
        if (effect.shader) SDL_ReleaseGPUShader(device, effect.shader);
    }
    m_customEffects = std::move(candidate);
    return true;
#endif
}

bool Compositor2D::HasCustomEffect(const std::string_view id,
                                   const std::string_view target) const {
    const auto found = m_customEffects.find(std::string(id));
    return found != m_customEffects.end() &&
           (target.empty() || found->second.descriptor.targetLayer == target);
}

std::string Compositor2D::CustomEffectTarget(const std::string_view id) const {
    const auto found = m_customEffects.find(std::string(id));
    return found == m_customEffects.end()
               ? std::string{}
               : found->second.descriptor.targetLayer;
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
    if (custom == m_customEffects.end() ||
        custom->second.descriptor.targetLayer != "stage") {
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
    CustomUniformsV3 customUniforms{};
    customUniforms.texelSize[0] = 1.0f / static_cast<float>(m_width);
    customUniforms.texelSize[1] = 1.0f / static_cast<float>(m_height);
    customUniforms.progress = std::clamp(effects.customProgress, 0.0f, 1.0f);
    customUniforms.randomSeed = effects.randomSeed;
    for (std::size_t slot = 0; slot < effects.customParameters.size(); ++slot)
        std::ranges::copy(effects.customParameters[slot],
                          customUniforms.parameters[slot]);
    customUniforms.viewport[2] = static_cast<float>(m_width);
    customUniforms.viewport[3] = static_cast<float>(m_height);
    const std::uint32_t uniformBytes =
        custom->second.descriptor.schemaRevision >= 3
            ? sizeof(CustomUniformsV3)
            : sizeof(CustomUniformsV2);
    (void)ApplyRenderState(custom->second.state, customSource,
                           m_previousTarget, &customUniforms,
                           uniformBytes);
}

bool Compositor2D::BeginNodeEffect(
    const std::string_view id, const float progress, const float randomSeed,
    const std::array<std::array<float, 4>, 8>& parameters,
    const std::array<float, 4>& viewport) {
#if defined(__EMSCRIPTEN__)
    (void)id; (void)progress; (void)randomSeed; (void)parameters; (void)viewport;
    return false;
#else
    const auto found = m_customEffects.find(std::string(id));
    if (found == m_customEffects.end() ||
        found->second.descriptor.targetLayer != "node" ||
        !SetCustomUniforms(m_renderer, found->second.state,
                           found->second.descriptor, m_width, m_height,
                           progress, randomSeed, parameters, viewport))
        return false;
    return SDL_SetGPURenderState(m_renderer, found->second.state);
#endif
}

void Compositor2D::EndNodeEffect() {
#if !defined(__EMSCRIPTEN__)
    (void)SDL_SetGPURenderState(m_renderer, nullptr);
#endif
}

bool Compositor2D::ApplyCustomTransition(
    const std::string_view id, SDL_Texture* outgoing, SDL_Texture* incoming,
    const float progress, const float randomSeed,
    const std::array<std::array<float, 4>, 8>& parameters) {
#if defined(__EMSCRIPTEN__)
    (void)id; (void)outgoing; (void)incoming; (void)progress;
    (void)randomSeed; (void)parameters;
    return false;
#else
    auto found = m_customEffects.find(std::string(id));
    if (found == m_customEffects.end() || !outgoing || !incoming ||
        found->second.descriptor.targetLayer != "transition")
        return false;
    SDL_GPUTexture* incomingGpu = static_cast<SDL_GPUTexture*>(
        SDL_GetPointerProperty(SDL_GetTextureProperties(incoming),
                               SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, nullptr));
    if (!incomingGpu) return false;
    SDL_GPURenderState* state = nullptr;
    if (const auto cached = found->second.transitionStates.find(incomingGpu);
        cached != found->second.transitionStates.end()) {
        state = cached->second;
    } else {
        const SDL_GPUTextureSamplerBinding binding{
            incomingGpu, found->second.transitionSampler};
        SDL_GPURenderStateCreateInfo stateInfo{};
        stateInfo.fragment_shader = found->second.shader;
        stateInfo.num_sampler_bindings = 1;
        stateInfo.sampler_bindings = &binding;
        state = SDL_CreateGPURenderState(m_renderer, &stateInfo);
        if (!state) return false;
        found->second.transitionStates.emplace(incomingGpu, state);
    }
    const std::array<float, 4> viewport{
        0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height)};
    if (!SetCustomUniforms(m_renderer, state, found->second.descriptor,
                           m_width, m_height, progress, randomSeed, parameters,
                           viewport) ||
        !SDL_SetGPURenderState(m_renderer, state))
        return false;
    const SDL_FRect destination{0.0f, 0.0f, static_cast<float>(m_width),
                                static_cast<float>(m_height)};
    const bool rendered =
        SDL_RenderTexture(m_renderer, outgoing, nullptr, &destination);
    (void)SDL_SetGPURenderState(m_renderer, nullptr);
    return rendered;
#endif
}

void Compositor2D::InvalidateCustomTransitionTargets() {
#if !defined(__EMSCRIPTEN__)
    for (auto& [_, effect] : m_customEffects) {
        for (auto& [__, state] : effect.transitionStates)
            if (state) SDL_DestroyGPURenderState(state);
        effect.transitionStates.clear();
    }
#endif
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
