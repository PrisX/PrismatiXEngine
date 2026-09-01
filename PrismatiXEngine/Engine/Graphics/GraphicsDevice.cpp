#include "Engine/Graphics/GraphicsDevice.h"

#include "Engine/Support/Logger.h"

#include <SDL3/SDL.h>

namespace px::graphics {

GraphicsDevice::~GraphicsDevice() { Destroy(); }

bool GraphicsDevice::Create(SDL_Window* window,
                            const GraphicsTier requiredTier) {
    Destroy();
    if (!window) return false;
    if (requiredTier == GraphicsTier::GpuEffects) {
#if defined(__EMSCRIPTEN__)
        PX_LOG_ERROR("SDL_GPU effects tier is unavailable in WASM Preview");
        return false;
#else
        constexpr SDL_GPUShaderFormat formats =
            SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL |
            SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB;
        m_gpu = SDL_CreateGPUDevice(formats, false, nullptr);
        if (!m_gpu) {
            PX_LOG_ERROR("SDL_CreateGPUDevice failed: {}", SDL_GetError());
            return false;
        }
        m_renderer = SDL_CreateGPURenderer(m_gpu, window);
        if (!m_renderer) {
            PX_LOG_ERROR("SDL_CreateGPURenderer failed: {}", SDL_GetError());
            Destroy();
            return false;
        }
        m_tier = GraphicsTier::GpuEffects;
        m_shaderFormats = SDL_GetGPUShaderFormats(m_gpu);
        if (const char* driver = SDL_GetGPUDeviceDriver(m_gpu)) m_driver = driver;
        PX_LOG_INFO("Graphics device created tier=gpu-effects driver={} shaderFormats=0x{:x}",
                    m_driver, m_shaderFormats);
        return true;
#endif
    }
    m_renderer = SDL_CreateRenderer(window, nullptr);
    if (!m_renderer) {
        PX_LOG_ERROR("SDL_CreateRenderer failed: {}", SDL_GetError());
        return false;
    }
    m_tier = GraphicsTier::Basic;
    if (const char* name = SDL_GetRendererName(m_renderer)) m_driver = name;
    PX_LOG_INFO("Graphics device created tier=basic driver={}", m_driver);
    return true;
}

void GraphicsDevice::Destroy() {
    if (m_renderer) SDL_DestroyRenderer(m_renderer);
    m_renderer = nullptr;
    if (m_gpu) SDL_DestroyGPUDevice(m_gpu);
    m_gpu = nullptr;
    m_tier = GraphicsTier::Basic;
    m_shaderFormats = 0;
    m_driver.clear();
}

}  // namespace px::graphics
