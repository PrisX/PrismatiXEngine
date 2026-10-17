#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace px::platform {

// The process that started this one, when the platform can tell.
[[nodiscard]] std::optional<std::uint64_t> ParentProcessId();

// A probe that reports whether process `pid` is still running, or an empty
// function when it cannot be watched (no permission, unsupported platform).
// A `pid` that names a process created after this one was reused by the
// system: the original process is gone, and the probe reports that.
[[nodiscard]] std::function<bool()> ProcessAliveProbe(std::uint64_t pid);

// Calls `onExit` once, on a background thread, when `alive` turns false.
// Destroying the watch stops it without calling `onExit`. Used by tools that
// a developer process owns (PreviewHost, a Player started by `prismatix run`)
// so no window outlives the process that controls it.
class ProcessExitWatch {
public:
    [[nodiscard]] static std::unique_ptr<ProcessExitWatch> Start(
        std::function<bool()> alive, std::function<void()> onExit);
    ~ProcessExitWatch();
    ProcessExitWatch(const ProcessExitWatch&) = delete;
    ProcessExitWatch& operator=(const ProcessExitWatch&) = delete;

private:
    struct State;
    explicit ProcessExitWatch(std::unique_ptr<State> state);
    std::unique_ptr<State> m_state;
};

}  // namespace px::platform
