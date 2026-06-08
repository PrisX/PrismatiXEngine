#pragma once

#include "Engine/Audio/AudioEngine.h"
#include "Engine/Common/Types.h"
#include "Engine/Gfx/AssetCache.h"
#include "Engine/Gfx/Renderer2D.h"
#include "Engine/IO/Vfs.h"
#include "Engine/Platform/Clock.h"
#include "Engine/Platform/Input.h"
#include "Engine/Platform/Window.h"

#include <memory>
#include <string>
#include <vector>

namespace px {

struct RuntimeConfig {
    std::string title = "PrismatiX";
    int width = 1280;
    int height = 720;
    int logicalWidth = 1280;
    int logicalHeight = 720;
    bool resizable = true;
    Color clearColor = Color{ 12, 14, 20, 255 };

    std::vector<std::string> mountDirs = { "." };
    std::vector<std::string> mountArchives;
    std::string archiveKey;
};

class Runtime {
public:
    Runtime() = default;
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    bool Init(const RuntimeConfig& config);
    void Shutdown();

    bool BeginFrame();
    void EndFrame();

    [[nodiscard]] Window& GetWindow() { return m_window; }
    [[nodiscard]] Input& GetInput() { return m_input; }
    [[nodiscard]] Clock& GetClock() { return m_clock; }
    [[nodiscard]] io::Vfs& Vfs() { return m_vfs; }
    [[nodiscard]] gfx::AssetCache& Assets() { return *m_assets; }
    [[nodiscard]] gfx::Renderer2D& Renderer() { return *m_renderer; }
    [[nodiscard]] audio::AudioEngine& Audio() { return *m_audio; }
    [[nodiscard]] const RuntimeConfig& Config() const { return m_config; }

private:
    RuntimeConfig m_config;
    Window m_window;
    Input m_input;
    Clock m_clock;
    io::Vfs m_vfs;
    std::unique_ptr<gfx::AssetCache> m_assets;
    std::unique_ptr<gfx::Renderer2D> m_renderer;
    std::unique_ptr<audio::AudioEngine> m_audio;
    bool m_initialized = false;
    bool m_running = false;
};

}
