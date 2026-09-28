#include "Engine/Core/Clock.h"

#include <SDL3/SDL.h>

namespace px {

namespace {
constexpr float kMaxDelta = 0.1f;
}

void Clock::Tick() {
    // Millisecond ticks quantize a 60 Hz delta to 16 or 17 ms, which shows as
    // uneven motion in slow pans and fades. Deltas come from the ns counter.
    const std::uint64_t nowNs = SDL_GetTicksNS();
    if (m_lastNs == 0) {
        m_lastNs = nowNs;
    }
    float delta = static_cast<float>(static_cast<double>(nowNs - m_lastNs) / 1e9);
    if (delta > kMaxDelta) {
        delta = kMaxDelta;
    }
    m_delta = delta;
    m_lastNs = nowNs;
    m_nowMs = nowNs / 1'000'000u;
    ++m_frames;
}

}
