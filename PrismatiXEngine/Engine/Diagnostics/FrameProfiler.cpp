#include "Engine/Diagnostics/FrameProfiler.h"

#include "Engine/Core/NumberParsing.h"
#include "Engine/Support/Logger.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <charconv>

namespace px::diag {

namespace {
constexpr std::array<std::string_view, 7> kPhaseNames{
    "events", "script", "logic", "stage", "uiUpdate", "uiRender", "present"};

double Ms(const std::uint64_t nanoseconds) {
    return static_cast<double>(nanoseconds) / 1'000'000.0;
}
}  // namespace

FrameProfiler& FrameProfiler::Instance() {
    static FrameProfiler profiler;
    return profiler;
}

FrameProfiler::FrameProfiler() {
    const char* value = SDL_getenv("PRISMATIX_PERF");
    m_enabled = value && *value && std::string_view(value) != "0";
    if (!m_enabled) return;
    const std::string_view text(value);
    double threshold = 0.0;
    if (ParseFiniteDouble(text, threshold) && threshold > 1.0) m_hitchMs = threshold;
    PX_LOG_INFO("Frame profiler enabled hitchMs={}", m_hitchMs);
}

void FrameProfiler::BeginFrame() {
    if (!m_enabled) return;
    m_frameStart = SDL_GetTicksNS();
    if (m_windowStart == 0) m_windowStart = m_frameStart;
    m_frame.fill(0);
    m_notes.clear();
}

void FrameProfiler::EndFrame() {
    if (!m_enabled || m_frameStart == 0) return;
    const std::uint64_t now = SDL_GetTicksNS();
    const std::uint64_t total = now - m_frameStart;
    ++m_windowFrames;
    m_windowFrameTotal += total;
    m_windowFrameMax = std::max(m_windowFrameMax, total);
    for (std::size_t i = 0; i < kPhases; ++i) {
        m_windowTotal[i] += m_frame[i];
        m_windowMax[i] = std::max(m_windowMax[i], m_frame[i]);
    }
    // Present includes the vsync wait, so a hitch is judged on work only.
    const std::uint64_t work =
        total - m_frame[static_cast<std::size_t>(FramePhase::Present)];
    if (Ms(work) >= m_hitchMs) {
        ++m_windowHitches;
        std::string phases;
        for (std::size_t i = 0; i < kPhases; ++i)
            phases += fmt::format(" {}={:.2f}", kPhaseNames[i], Ms(m_frame[i]));
        PX_LOG_WARN("Frame hitch work={:.2f}ms total={:.2f}ms{}{}{}", Ms(work),
                    Ms(total), phases, m_notes.empty() ? "" : " notes=",
                    m_notes);
    }
    if (now - m_windowStart >= 1'000'000'000ull) {
        std::string phases;
        for (std::size_t i = 0; i < kPhases; ++i)
            phases += fmt::format(" {}={:.2f}/{:.2f}", kPhaseNames[i],
                                  Ms(m_windowTotal[i]) / static_cast<double>(m_windowFrames),
                                  Ms(m_windowMax[i]));
        PX_LOG_INFO("Frame stats fps={} avg={:.2f}ms max={:.2f}ms hitches={} avg/max:{}",
                    m_windowFrames,
                    Ms(m_windowFrameTotal) / static_cast<double>(m_windowFrames),
                    Ms(m_windowFrameMax), m_windowHitches, phases);
        m_windowStart = now;
        m_windowFrames = 0;
        m_windowFrameTotal = 0;
        m_windowFrameMax = 0;
        m_windowHitches = 0;
        m_windowTotal.fill(0);
        m_windowMax.fill(0);
    }
    m_frameStart = 0;
}

void FrameProfiler::Add(const FramePhase phase, const std::uint64_t nanoseconds) {
    if (!m_enabled) return;
    m_frame[static_cast<std::size_t>(phase)] += nanoseconds;
}

void FrameProfiler::Note(const std::string_view note) {
    if (!m_enabled) return;
    if (!m_notes.empty()) m_notes += ',';
    m_notes += note;
}

ScopedFramePhase::ScopedFramePhase(const FramePhase phase) : m_phase(phase) {
    if (FrameProfiler::Instance().Enabled()) m_start = SDL_GetTicksNS();
}

ScopedFramePhase::~ScopedFramePhase() {
    if (m_start != 0)
        FrameProfiler::Instance().Add(m_phase, SDL_GetTicksNS() - m_start);
}

}  // namespace px::diag
