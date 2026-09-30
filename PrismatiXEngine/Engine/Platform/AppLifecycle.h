#pragma once

#include <cstdint>
#include <functional>
#include <optional>

namespace px::platform {

// OS application lifecycle, as SDL reports it on Android and iOS. Desktop
// platforms never send these, so everything here stays idle on Windows,
// macOS and Linux.
enum class LifecycleEvent : std::uint8_t {
    WillEnterBackground,
    DidEnterBackground,
    WillEnterForeground,
    DidEnterForeground,
    LowMemory,
    Terminating,
};

enum class PersistReason : std::uint8_t {
    // The app is leaving the foreground. The OS may kill it later without
    // another callback, so this is the last reliable chance to save.
    Background,
    // The OS is ending the process now.
    Terminating,
};

// What the engine and the game do in response. Every callback is optional.
struct LifecycleActions {
    // Pause audio and video that is audible now; resume exactly that later.
    std::function<void()> suspendMedia;
    std::function<void()> resumeMedia;
    // Drop caches that can be rebuilt from package data.
    std::function<void()> trimCaches;
};

// The rules for reacting to lifecycle events, independent of SDL event
// pumping so they can be unit tested:
//  - Background: persist first (so the save still sees live audio and video
//    state), then suspend media. Will/Did pairs act once.
//  - Foreground: resume media once, and report the resume so the frame clock
//    can restart instead of seeing a huge delta.
//  - Low memory: trim caches every time.
//  - Terminating: persist unless nothing could have changed since the
//    background save, then request quit.
// A throwing callback is contained; it never stops later steps.
class AppLifecycle {
public:
    using PersistHandler = std::function<void(PersistReason)>;

    void SetActions(LifecycleActions actions) { m_actions = std::move(actions); }
    // The game's save path: an automatic save plus settings/profile flush.
    // Called synchronously on the thread that pumps SDL events.
    void SetPersistHandler(PersistHandler handler) {
        m_persist = std::move(handler);
    }

    void Handle(LifecycleEvent event);

    // True between entering the background and returning to the foreground.
    // Nothing may be rendered while suspended.
    [[nodiscard]] bool Suspended() const { return m_background; }
    [[nodiscard]] bool TerminateRequested() const { return m_terminating; }
    // True once after each return to the foreground.
    [[nodiscard]] bool ConsumeResumed();

    [[nodiscard]] std::uint32_t PersistCount() const { return m_persistCount; }
    [[nodiscard]] std::uint32_t TrimCount() const { return m_trimCount; }

    // Maps an SDL_EventType value to a lifecycle event; nullopt for others.
    [[nodiscard]] static std::optional<LifecycleEvent> FromSdlEventType(
        std::uint32_t type);

private:
    void EnterBackground();
    void Persist(PersistReason reason);

    LifecycleActions m_actions;
    PersistHandler m_persist;
    bool m_background = false;
    bool m_persistedInBackground = false;
    bool m_resumed = false;
    bool m_terminating = false;
    std::uint32_t m_persistCount = 0;
    std::uint32_t m_trimCount = 0;
};

}  // namespace px::platform
