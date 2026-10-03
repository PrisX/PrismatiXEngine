#include "Engine/Runtime.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <utility>

#if !defined(PRISMATIX_PREVIEW_WASM)
#include "Engine/IO/Crypto.h"
#endif
#include "Engine/Diagnostics/FrameProfiler.h"
#if !defined(PRISMATIX_PREVIEW_WASM)
#include "Engine/Video/VideoPlayer.h"
#endif
#include "Engine/Support/Logger.h"
#if defined(PRISMATIX_WEB_PLAYER)
#include <emscripten/emscripten.h>
EM_JS(bool, PrismatiXWebHidden, (), { return document.hidden; });
#endif

namespace px {

Runtime::~Runtime() { Shutdown(); }

bool Runtime::Init(const RuntimeConfig& config) {
    m_config = config;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS)) {
        PX_LOG_ERROR("SDL_Init failed: {}", SDL_GetError());
        return false;
    }
    // Android and iOS deliver lifecycle events synchronously from inside
    // event pumping (on the thread that pumps), before the OS suspends the
    // app; a polled copy could arrive only after resuming. Desktop platforms
    // never send them.
#if !defined(PRISMATIX_WEB_PLAYER)
    m_lifecycleWatch = SDL_AddEventWatch(&Runtime::LifecycleEventWatch, this);
#endif
    // Browser beforeunload callbacks cannot await IndexedDB and may run while
    // the main Asyncify frame is suspended. Web lifecycle persistence instead
    // runs at the normal frame boundary when document visibility changes.
    if (!TTF_Init()) {
        PX_LOG_ERROR("TTF_Init failed: {}", SDL_GetError());
        SDL_Quit();
        return false;
    }

    const graphics::GraphicsTier graphicsTier =
        m_config.graphicsTier == "gpu-effects"
            ? graphics::GraphicsTier::GpuEffects
            : graphics::GraphicsTier::Basic;
    if (!m_window.Create(m_config.title, m_config.width, m_config.height,
                         m_config.resizable, graphicsTier)) {
        TTF_Quit();
        SDL_Quit();
        return false;
    }

    // Fingers use the same logical coordinates as the mouse.
    m_input.SetTouchCoordinateMapper(
        [this](const float normalizedX, const float normalizedY, float& x,
               float& y) {
            SDL_Window* window = m_window.Handle();
            SDL_Renderer* renderer = m_window.Renderer();
            int width = 0, height = 0;
            return window && renderer &&
                   SDL_GetWindowSize(window, &width, &height) &&
                   SDL_RenderCoordinatesFromWindow(
                       renderer, normalizedX * static_cast<float>(width),
                       normalizedY * static_cast<float>(height), &x, &y);
        });
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
#if !defined(PRISMATIX_PREVIEW_WASM)
    for (const auto& overlay : m_config.overlays) {
        if (!m_vfs.MountArchive(overlay.archive, nullptr,
                                io::ArchiveMountPriority::Overlay,
                                overlay.tombstones)) {
            PX_LOG_ERROR("Runtime initialization failed: verified overlay '{}' could not be mounted",
                         overlay.archive);
            Shutdown();
            return false;
        }
    }
#else
    if (!m_config.overlays.empty()) {
        PX_LOG_ERROR("Runtime initialization rejected an overlay in WASM preview");
        Shutdown();
        return false;
    }
#endif
    m_assets = std::make_unique<graphics::AssetCache>(m_window.Renderer(), m_vfs);
    m_assets->SetAsyncPreloadEnabled(config.asyncAssetPreload);
    m_renderer = std::make_unique<graphics::Renderer2D>(
        m_window.Renderer(), *m_assets,
        graphicsTier == graphics::GraphicsTier::GpuEffects);
    if (!m_renderer->Ready()) {
        PX_LOG_ERROR("Runtime initialization failed: required GPU compositor shader is unavailable");
        Shutdown();
        return false;
    }
    if (!m_renderer->LoadCustomEffects(config.customEffects, m_vfs)) {
        PX_LOG_ERROR("Runtime initialization failed: packaged custom effect artifacts are invalid or unsupported");
        Shutdown();
        return false;
    }
    m_renderer->SetLogicalSize(m_config.logicalWidth, m_config.logicalHeight);

    if (m_config.logicalHeight > 0) {
        m_window.SetAspectRatio(static_cast<float>(m_config.logicalWidth) / static_cast<float>(m_config.logicalHeight));
    }
    m_audio = std::make_unique<audio::AudioEngine>(m_vfs);
    m_audio->Init();
    m_lifecycle.SetActions({
        .suspendMedia = [this] {
            if (m_audio) m_audio->SetSuspended(true);
#if !defined(PRISMATIX_PREVIEW_WASM)
            video::VideoPlayer::SetAllSuspended(true);
#endif
        },
        .resumeMedia = [this] {
            if (m_audio) m_audio->SetSuspended(false);
#if !defined(PRISMATIX_PREVIEW_WASM)
            video::VideoPlayer::SetAllSuspended(false);
#endif
        },
        .trimCaches = [this] {
            if (m_assets) (void)m_assets->TrimReclaimable();
            if (m_audio) (void)m_audio->TrimReclaimable();
        },
    });

    m_initialized = true;
    m_running = true;
    PX_LOG_INFO("Runtime initialized.");
    return true;
}

void Runtime::Shutdown() {
    if (!m_initialized && !m_audio && !m_renderer && !m_assets &&
        !m_window.Valid() && SDL_WasInit(0) == 0) {
        return;
    }
    if (m_lifecycleWatch) {
        SDL_RemoveEventWatch(&Runtime::LifecycleEventWatch, this);
        m_lifecycleWatch = false;
    }
    m_lifecycle.SetActions({});
    m_audio.reset();
    m_renderer.reset();
    m_assets.reset();
    m_vfs.Clear();
    m_window.Destroy();
    if (TTF_WasInit()) TTF_Quit();
    if (SDL_WasInit(0) != 0) SDL_Quit();
    m_initialized = false;
    m_running = false;
    PX_LOG_INFO("Runtime shut down.");
}

bool Runtime::BeginFrame() {
    if (!m_running) {
        return false;
    }
    diag::FrameProfiler::Instance().BeginFrame();
    const diag::ScopedFramePhase profiled(diag::FramePhase::Events);
    m_input.NewFrame();
#if defined(PRISMATIX_WEB_PLAYER)
    m_lifecycle.Handle(PrismatiXWebHidden() ? platform::LifecycleEvent::DidEnterBackground
                                         : platform::LifecycleEvent::DidEnterForeground);
#endif
    SDL_Event event;
    bool resized = false;
    while (SDL_PollEvent(&event)) HandleEvent(event, resized);
    // Nothing may be drawn while the app is in the background: the surface
    // can be gone. Android's SDL glue already blocks inside the pump while
    // paused; this also covers platforms that keep pumping.
    while (m_lifecycle.Suspended() && !m_input.QuitRequested() &&
           !m_lifecycle.TerminateRequested()) {
#if defined(PRISMATIX_WEB_PLAYER)
        emscripten_sleep(16);
        m_lifecycle.Handle(PrismatiXWebHidden() ? platform::LifecycleEvent::DidEnterBackground
                                             : platform::LifecycleEvent::DidEnterForeground);
        while (SDL_PollEvent(&event)) HandleEvent(event, resized);
#else
        if (SDL_WaitEventTimeout(&event, 250)) HandleEvent(event, resized);
#endif
    }
    m_input.UpdateTouch(SDL_GetTicksNS());
    if (m_lifecycle.ConsumeResumed()) {
        // The swapchain may have been recreated at a new size, and the time
        // spent suspended is not frame time.
        resized = true;
        m_clock.Restart();
    }
    if (resized && m_renderer) m_renderer->UpdateTextDensity();
    if (m_input.QuitRequested() || m_lifecycle.TerminateRequested()) {
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
    m_renderer->UpdateScreenEffects(m_clock.DeltaSeconds());
    if (!m_renderer->BeginFrame(m_config.clearColor))
        m_window.Clear(m_config.clearColor);
    return true;
}

bool Runtime::LifecycleEventWatch(void* userdata, SDL_Event* event) {
    auto* runtime = static_cast<Runtime*>(userdata);
    if (!runtime || !event) return true;
    if (const auto lifecycle =
            platform::AppLifecycle::FromSdlEventType(event->type))
        runtime->m_lifecycle.Handle(*lifecycle);
    return true;
}

void Runtime::HandleEvent(SDL_Event& event, bool& resized) {
    switch (event.type) {
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        resized = true;
        break;
    case SDL_EVENT_RENDER_TARGETS_RESET:
        if (m_renderer) m_renderer->ResetRenderTargets();
        break;
    case SDL_EVENT_RENDER_DEVICE_RESET:
        if (m_assets) m_assets->ReleaseDeviceTextures();
        if (m_renderer) m_renderer->ResetRenderTargets();
        break;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        // Input maps the window-normalized finger position itself (see the
        // touch mapper in Init); converting here as well scaled it twice.
        m_input.Process(event);
        return;
    default:
        break;
    }
    SDL_ConvertEventToRenderCoordinates(m_window.Renderer(), &event);
    m_input.Process(event);
}

void Runtime::EndFrame() {
    {
        const diag::ScopedFramePhase profiled(diag::FramePhase::Present);
        if (m_renderer) m_renderer->EndFrame();
        if (m_composedFrame) std::exchange(m_composedFrame, nullptr)();
        m_window.Present();
    }
    diag::FrameProfiler::Instance().EndFrame();
}

}  // namespace px
