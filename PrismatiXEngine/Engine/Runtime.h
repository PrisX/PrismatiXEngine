#pragma once

#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/Types.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Graphics/CustomEffect.h"
#include "Engine/IO/VFS.h"
#include "Engine/Core/Clock.h"
#include "Engine/Platform/AppLifecycle.h"
#include "Engine/Platform/Input.h"
#include "Engine/Platform/Window.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

union SDL_Event;

namespace px {

struct RuntimeOverlayMount {
    std::string archive;
    std::vector<std::string> tombstones;
};

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
    std::vector<RuntimeOverlayMount> overlays;
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
    // Runs once, after the frame (including screen transitions) is composited
    // and before it is presented; used to capture exactly what players see.
    void OnNextComposedFrame(std::function<void()> callback) {
        m_composedFrame = std::move(callback);
    }

    [[nodiscard]] Window& GetWindow() { return m_window; }
    [[nodiscard]] Input& GetInput() { return m_input; }
    [[nodiscard]] Clock& GetClock() { return m_clock; }
    [[nodiscard]] io::VFS& VFS() { return m_vfs; }
    [[nodiscard]] graphics::AssetCache& Assets() { return *m_assets; }
    [[nodiscard]] graphics::Renderer2D& Renderer() { return *m_renderer; }
    [[nodiscard]] audio::AudioEngine& Audio() { return *m_audio; }
    [[nodiscard]] const RuntimeConfig& Config() const { return m_config; }
    // Android/iOS lifecycle. The Runtime pauses audio and video, trims caches
    // and stops rendering while suspended; the game only supplies its save
    // path, which runs on this thread before media is paused.
    void SetLifecyclePersistHandler(platform::AppLifecycle::PersistHandler handler) {
        m_lifecycle.SetPersistHandler(std::move(handler));
    }
    [[nodiscard]] platform::AppLifecycle& Lifecycle() { return m_lifecycle; }

private:
    static bool LifecycleEventWatch(void* userdata, SDL_Event* event);
    void HandleEvent(SDL_Event& event, bool& resized);

    RuntimeConfig m_config;
    Window m_window;
    Input m_input;
    Clock m_clock;
    io::VFS m_vfs;
    std::unique_ptr<graphics::AssetCache> m_assets;
    std::unique_ptr<graphics::Renderer2D> m_renderer;
    std::unique_ptr<audio::AudioEngine> m_audio;
    std::function<void()> m_composedFrame;
    platform::AppLifecycle m_lifecycle;
    bool m_lifecycleWatch = false;
    bool m_initialized = false;
    bool m_running = false;
};

}
