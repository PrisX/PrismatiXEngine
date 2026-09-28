#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace px::diag {

// Coarse per-frame phase timing for the Player loop. Disabled unless the
// PRISMATIX_PERF environment variable is set, in which case it logs one
// summary per second and every frame slower than the hitch threshold.
enum class FramePhase : std::uint8_t {
    Events,
    Script,
    Logic,
    StageRender,
    UiUpdate,
    UiRender,
    Present,
    Count,
};

class FrameProfiler {
public:
    static FrameProfiler& Instance();

    [[nodiscard]] bool Enabled() const { return m_enabled; }
    void BeginFrame();
    void EndFrame();
    void Add(FramePhase phase, std::uint64_t nanoseconds);
    // Attaches a short note to the current frame, printed if it hitches.
    void Note(std::string_view note);

private:
    FrameProfiler();

    static constexpr std::size_t kPhases =
        static_cast<std::size_t>(FramePhase::Count);
    bool m_enabled = false;
    double m_hitchMs = 25.0;
    std::uint64_t m_frameStart = 0;
    std::uint64_t m_windowStart = 0;
    std::array<std::uint64_t, kPhases> m_frame{};
    std::array<std::uint64_t, kPhases> m_windowTotal{};
    std::array<std::uint64_t, kPhases> m_windowMax{};
    std::uint64_t m_windowFrames = 0;
    std::uint64_t m_windowFrameMax = 0;
    std::uint64_t m_windowFrameTotal = 0;
    std::uint64_t m_windowHitches = 0;
    std::string m_notes;
};

class ScopedFramePhase {
public:
    explicit ScopedFramePhase(FramePhase phase);
    ~ScopedFramePhase();
    ScopedFramePhase(const ScopedFramePhase&) = delete;
    ScopedFramePhase& operator=(const ScopedFramePhase&) = delete;

private:
    FramePhase m_phase;
    std::uint64_t m_start = 0;
};

}  // namespace px::diag
