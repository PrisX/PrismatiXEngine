#pragma once

#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/Types.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Graphics/CustomEffect.h"
#include "Engine/IO/VFS.h"
#include "Engine/Core/Clock.h"
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
    // Platform composition capability. Single-thread targets use deterministic
    // main-thread texture preload instead of std::async.
    bool asyncAssetPreload = true;
    std::string graphicsTier = "basic";
    std::vector<graphics::CustomEffectDescriptor> customEffects;

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
    [[nodiscard]] io::VFS& VFS() { return m_vfs; }
    [[nodiscard]] graphics::AssetCache& Assets() { return *m_assets; }
    [[nodiscard]] graphics::Renderer2D& Renderer() { return *m_renderer; }
    [[nodiscard]] audio::AudioEngine& Audio() { return *m_audio; }
    [[nodiscard]] const RuntimeConfig& Config() const { return m_config; }

private:
    RuntimeConfig m_config;
    Window m_window;
    Input m_input;
    Clock m_clock;
    io::VFS m_vfs;
    std::unique_ptr<graphics::AssetCache> m_assets;
    std::unique_ptr<graphics::Renderer2D> m_renderer;
    std::unique_ptr<audio::AudioEngine> m_audio;
    bool m_initialized = false;
    bool m_running = false;
};

}
