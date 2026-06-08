#pragma once

#include <cstdint>

namespace px {

class Clock {
public:
    void Tick();

    [[nodiscard]] float DeltaSeconds() const { return m_delta; }
    [[nodiscard]] std::uint64_t NowMs() const { return m_nowMs; }
    [[nodiscard]] std::uint64_t FrameCount() const { return m_frames; }

private:
    std::uint64_t m_lastMs = 0;
    std::uint64_t m_nowMs = 0;
    std::uint64_t m_frames = 0;
    float m_delta = 0.0f;
};

}
