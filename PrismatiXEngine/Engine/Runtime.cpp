#include "Engine/Runtime.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#if !defined(PRISMATIX_PREVIEW_WASM)
#include "Engine/IO/Crypto.h"
#endif
#include "Engine/Support/Logger.h"

namespace px {

Runtime::~Runtime() { Shutdown(); }

bool Runtime::Init(const RuntimeConfig& config) {
    m_config = config;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        PX_LOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        return false;
    }
    if (!TTF_Init()) {
        PX_LOG_ERROR("TTF_Init failed: {}", SDL_GetError());
        SDL_Quit();
        return false;
    }

    if (!m_window.Create(m_config.title, m_config.width, m_config.height, m_config.resizable)) {
        TTF_Quit();
        SDL_Quit();
        return false;
    }

    for (const std::string& dir : m_config.mountDirs) {
        m_vfs.MountDirectory(dir);
    }
    for (const std::string& archive : m_config.mountArchives) {
#if defined(PRISMATIX_PREVIEW_WASM)
        (void)archive;
        PX_LOG_ERROR("Runtime initialization rejected an archive mount in WASM preview");
        Shutdown();
        return false;
#else
        bool mounted = false;
        if (m_config.archiveKey.empty()) {
            mounted = m_vfs.MountArchive(archive);
        }
        else {
            const crypto::Key key = crypto::DeriveKey(m_config.archiveKey);
            mounted = m_vfs.MountArchive(archive, &key);
        }
        if (!mounted) {
            PX_LOG_ERROR("Runtime initialization failed: content group '{}' could not be mounted", archive);
            Shutdown();
            return false;
        }
#endif
    }
    m_assets = std::make_unique<graphics::AssetCache>(m_window.Renderer(), m_vfs);
    m_assets->SetAsyncPreloadEnabled(config.asyncAssetPreload);
    m_renderer = std::make_unique<graphics::Renderer2D>(m_window.Renderer(), *m_assets);
    m_renderer->SetLogicalSize(m_config.logicalWidth, m_config.logicalHeight);

    if (m_config.logicalHeight > 0) {
        m_window.SetAspectRatio(static_cast<float>(m_config.logicalWidth) / static_cast<float>(m_config.logicalHeight));
    }
    m_audio = std::make_unique<audio::AudioEngine>(m_vfs);
    m_audio->Init();

    m_initialized = true;
    m_running = true;
    PX_LOG_INFO("Runtime initialized.");
    return true;
}

void Runtime::Shutdown() {
    if (!m_initialized) {
        return;
    }
    m_audio.reset();
    m_renderer.reset();
    m_assets.reset();
    m_vfs.Clear();
    m_window.Destroy();
    TTF_Quit();
    SDL_Quit();
    m_initialized = false;
    m_running = false;
    PX_LOG_INFO("Runtime shut down.");
}

bool Runtime::BeginFrame() {
    if (!m_running) {
        return false;
    }
    m_input.NewFrame();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        SDL_ConvertEventToRenderCoordinates(m_window.Renderer(), &event);
        m_input.Process(event);
    }
    if (m_input.QuitRequested()) {
        m_running = false;
        return false;
    }
    m_clock.Tick();
    if (m_assets) {
        m_assets->BeginFrame();
    }
    if (m_audio) {
        m_audio->Update();
    }
    m_window.Clear(m_config.clearColor);
    return true;
}

void Runtime::EndFrame() { m_window.Present(); }

}  // namespace px
