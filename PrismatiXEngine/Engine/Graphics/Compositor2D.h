#pragma once

#include "Engine/Graphics/CustomEffect.h"

#include <array>
#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <vector>

namespace px::io { class VFS; }

struct SDL_Renderer;
struct SDL_GPUShader;
struct SDL_GPURenderState;
struct SDL_Texture;

namespace px::graphics {

struct StagePostEffects {
    float blur = 0.0f;
    float vignette = 0.0f;
    float colorGrade = 0.0f;
    float randomSeed = 0.0f;
    std::string customEffect;
    float customProgress = 0.0f;
    std::array<std::array<float, 4>, 8> customParameters{};
};

// Ordered Stage compositor. Background/actors/layers render into one target;
// post effects are resolved before dialogue/UI is drawn to the output target.
class Compositor2D final {
public:
    Compositor2D(SDL_Renderer* renderer, bool enabled);
    ~Compositor2D();
    Compositor2D(const Compositor2D&) = delete;
    Compositor2D& operator=(const Compositor2D&) = delete;

    void SetLogicalSize(int width, int height);
    bool LoadCustomEffects(const std::vector<CustomEffectDescriptor>& effects,
                           const io::VFS& vfs);
    [[nodiscard]] bool HasCustomEffect(std::string_view id) const;
    [[nodiscard]] std::vector<std::string> CustomEffectIds() const;
    [[nodiscard]] std::optional<std::array<std::array<float, 4>, 8>>
    CustomEffectDefaults(std::string_view id) const;
    [[nodiscard]] bool BeginStage();
    void EndStage(const StagePostEffects& effects);
    [[nodiscard]] bool Enabled() const { return m_enabled; }
    [[nodiscard]] bool Ready() const { return !m_required || m_enabled; }

private:
    bool CreateGpuState();
    void DestroyGpuState();
    bool ApplyRenderState(SDL_GPURenderState* state, SDL_Texture* source,
                          SDL_Texture* target, const void* uniforms,
                          std::uint32_t uniformBytes);
    bool EnsureTarget();
    void DestroyTarget();

    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture* m_stage = nullptr;
    SDL_Texture* m_intermediate = nullptr;
    SDL_Texture* m_previousTarget = nullptr;
#if !defined(__EMSCRIPTEN__)
    SDL_GPUShader* m_shader = nullptr;
#endif
    SDL_GPURenderState* m_renderState = nullptr;
    struct CustomEffectState {
        CustomEffectDescriptor descriptor;
        SDL_GPUShader* shader = nullptr;
        SDL_GPURenderState* state = nullptr;
    };
    std::unordered_map<std::string, CustomEffectState> m_customEffects;
    int m_width = 0;
    int m_height = 0;
    bool m_enabled = false;
    bool m_required = false;
    bool m_recording = false;
};

}  // namespace px::graphics
