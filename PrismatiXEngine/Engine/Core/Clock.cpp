#include "Engine/Core/Clock.h"

#include <SDL3/SDL.h>

namespace px {

namespace {
constexpr float kMaxDelta = 0.1f;
}

void Clock::Tick() {
    const std::uint64_t now = SDL_GetTicks();
    if (m_lastMs == 0) {
        m_lastMs = now;
    }
    float delta = static_cast<float>(now - m_lastMs) / 1000.0f;
    if (delta > kMaxDelta) {
        delta = kMaxDelta;
    }
    m_delta = delta;
    m_lastMs = now;
    m_nowMs = now;
    ++m_frames;
}

}
