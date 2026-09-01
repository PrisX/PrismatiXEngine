#pragma once

#include <cstdint>
#include <string>

struct SDL_GPUDevice;
struct SDL_Renderer;
struct SDL_Window;

namespace px::graphics {

enum class GraphicsTier { Basic, GpuEffects };

// Owns the desktop graphics backend. The GPU tier deliberately creates an
// SDL_GPU device and passes that exact device to SDL's 2D GPU renderer so
// normal Stage/UI drawing and custom compositor passes share one queue.
class GraphicsDevice final {
public:
    GraphicsDevice() = default;
    ~GraphicsDevice();
    GraphicsDevice(const GraphicsDevice&) = delete;
    GraphicsDevice& operator=(const GraphicsDevice&) = delete;

    bool Create(SDL_Window* window, GraphicsTier requiredTier);
    void Destroy();

    [[nodiscard]] SDL_Renderer* Renderer() const { return m_renderer; }
    [[nodiscard]] SDL_GPUDevice* GPU() const { return m_gpu; }
    [[nodiscard]] GraphicsTier Tier() const { return m_tier; }
    [[nodiscard]] std::uint32_t ShaderFormats() const { return m_shaderFormats; }
    [[nodiscard]] const std::string& Driver() const { return m_driver; }

private:
    SDL_Renderer* m_renderer = nullptr;
    SDL_GPUDevice* m_gpu = nullptr;
    GraphicsTier m_tier = GraphicsTier::Basic;
    std::uint32_t m_shaderFormats = 0;
    std::string m_driver;
};

}  // namespace px::graphics
