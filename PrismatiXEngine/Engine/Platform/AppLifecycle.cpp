#include "Engine/Platform/AppLifecycle.h"

#include "Engine/Support/Logger.h"

#include <SDL3/SDL_events.h>

#include <exception>

namespace px::platform {

namespace {
void Invoke(const std::function<void()>& callback, const char* what) {
    if (!callback) return;
    try {
        callback();
    } catch (const std::exception& error) {
        PX_LOG_ERROR("Lifecycle {} failed: {}", what, error.what());
    } catch (...) {
        PX_LOG_ERROR("Lifecycle {} failed with an unknown exception", what);
    }
}
}  // namespace

void AppLifecycle::Handle(const LifecycleEvent event) {
    switch (event) {
    case LifecycleEvent::WillEnterBackground:
    case LifecycleEvent::DidEnterBackground:
        EnterBackground();
        return;
    case LifecycleEvent::WillEnterForeground:
        return;
    case LifecycleEvent::DidEnterForeground:
        if (!m_background) return;
        m_background = false;
        m_persistedInBackground = false;
        m_resumed = true;
        PX_LOG_INFO("Lifecycle: entered foreground; resuming media");
        Invoke(m_actions.resumeMedia, "media resume");
        return;
    case LifecycleEvent::LowMemory:
        ++m_trimCount;
        PX_LOG_WARN("Lifecycle: low memory; dropping reclaimable caches");
        Invoke(m_actions.trimCaches, "cache trim");
        return;
    case LifecycleEvent::Terminating:
        if (m_terminating) return;
        m_terminating = true;
        PX_LOG_INFO("Lifecycle: terminating");
        // Nothing runs between the background save and termination: the
        // game loop does not advance while suspended.
        if (!m_persistedInBackground) Persist(PersistReason::Terminating);
        return;
    }
}

void AppLifecycle::EnterBackground() {
    if (m_background) return;
    m_background = true;
    PX_LOG_INFO("Lifecycle: entering background; saving and pausing media");
    Persist(PersistReason::Background);
    m_persistedInBackground = true;
    Invoke(m_actions.suspendMedia, "media suspend");
}

void AppLifecycle::Persist(const PersistReason reason) {
    if (!m_persist) return;
    ++m_persistCount;
    Invoke([this, reason] { m_persist(reason); }, "persist");
}

bool AppLifecycle::ConsumeResumed() {
    const bool resumed = m_resumed;
    m_resumed = false;
    return resumed;
}

std::optional<LifecycleEvent> AppLifecycle::FromSdlEventType(
    const std::uint32_t type) {
    switch (type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        return LifecycleEvent::WillEnterBackground;
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        return LifecycleEvent::DidEnterBackground;
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
        return LifecycleEvent::WillEnterForeground;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        return LifecycleEvent::DidEnterForeground;
    case SDL_EVENT_LOW_MEMORY:
        return LifecycleEvent::LowMemory;
    case SDL_EVENT_TERMINATING:
        return LifecycleEvent::Terminating;
    default:
        return std::nullopt;
    }
}

}  // namespace px::platform
