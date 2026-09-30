#include "Engine/Platform/TouchGestures.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace px {

std::optional<TouchProfile> ParseTouchProfile(const std::string_view text) {
    if (text == "auto") return TouchProfile::Auto;
    if (text == "on") return TouchProfile::On;
    if (text == "off") return TouchProfile::Off;
    return std::nullopt;
}

TouchGestureRecognizer::Finger* TouchGestureRecognizer::Find(
    const std::uint64_t id) {
    if (m_primary.down && m_primary.id == id) return &m_primary;
    if (m_secondary.down && m_secondary.id == id) return &m_secondary;
    return nullptr;
}

bool TouchGestureRecognizer::AnyDown() const {
    return m_primary.down || m_secondary.down || m_extraFingers > 0;
}

void TouchGestureRecognizer::Emit(const TouchGesture gesture,
                                  const Finger& finger) {
    m_events.push_back({gesture, finger.startX, finger.startY});
}

void TouchGestureRecognizer::BecomeDrag(Finger& finger) {
    m_state = State::Dragging;
    Emit(TouchGesture::DragStart, finger);
}

void TouchGestureRecognizer::FinishIfAllUp() {
    m_state = AnyDown() ? State::Cancelled : State::Idle;
}

void TouchGestureRecognizer::Down(const std::uint64_t finger, const float x,
                                  const float y, const std::uint64_t ns) {
    const Finger touch{finger, x, y, x, y, ns, true, false};
    switch (m_state) {
    case State::Idle:
        m_primary = touch;
        m_secondary = {};
        m_extraFingers = 0;
        m_state = State::Pending;
        return;
    case State::Pending:
        m_secondary = touch;
        m_state = State::TwoFinger;
        return;
    case State::SwipeCandidate:
    case State::TwoFinger:
        ++m_extraFingers;
        m_state = State::Cancelled;
        return;
    case State::Dragging:
    case State::LongPressed:
    case State::Cancelled:
        // Extra fingers never interrupt a drag or a finished long press.
        ++m_extraFingers;
        return;
    }
}

void TouchGestureRecognizer::Move(const std::uint64_t finger, const float x,
                                  const float y, const std::uint64_t ns) {
    Finger* touch = Find(finger);
    if (!touch) return;
    touch->x = x;
    touch->y = y;
    const float dx = x - touch->startX;
    const float dy = y - touch->startY;
    if (std::hypot(dx, dy) > m_config.slop) touch->moved = true;
    switch (m_state) {
    case State::Pending:
        if (!touch->moved) return;
        if (dy < 0.0f && -dy >= std::fabs(dx) &&
            ns - touch->downNs <= m_config.swipeMaxNs)
            m_state = State::SwipeCandidate;
        else
            BecomeDrag(*touch);
        return;
    case State::SwipeCandidate:
        if (ns - touch->downNs > m_config.swipeMaxNs) BecomeDrag(*touch);
        return;
    case State::TwoFinger:
        if (touch->moved) m_state = State::Cancelled;
        return;
    default:
        return;
    }
}

void TouchGestureRecognizer::Up(const std::uint64_t finger, const float x,
                                const float y, const std::uint64_t ns) {
    Finger* touch = Find(finger);
    if (!touch) {
        if (m_extraFingers > 0) --m_extraFingers;
        if (m_state == State::Cancelled) FinishIfAllUp();
        return;
    }
    touch->x = x;
    touch->y = y;
    touch->down = false;
    const float dx = x - touch->startX;
    const float dy = y - touch->startY;
    switch (m_state) {
    case State::Pending:
        if (std::hypot(dx, dy) <= m_config.slop) Emit(TouchGesture::Tap, *touch);
        FinishIfAllUp();
        return;
    case State::SwipeCandidate:
        if (-dy >= m_config.swipeMinDistance && -dy >= 1.5f * std::fabs(dx) &&
            ns - touch->downNs <= m_config.swipeMaxNs)
            Emit(TouchGesture::SwipeUp, *touch);
        FinishIfAllUp();
        return;
    case State::TwoFinger:
        if (m_primary.down || m_secondary.down) return;
        if (ns - m_primary.downNs <= m_config.twoFingerTapNs &&
            !m_primary.moved && !m_secondary.moved)
            Emit(TouchGesture::TwoFingerTap, m_primary);
        FinishIfAllUp();
        return;
    case State::Dragging:
    case State::LongPressed:
        if (touch == &m_primary) FinishIfAllUp();
        return;
    case State::Cancelled:
        FinishIfAllUp();
        return;
    case State::Idle:
        return;
    }
}

void TouchGestureRecognizer::Cancel(const std::uint64_t finger) {
    if (Finger* touch = Find(finger)) touch->down = false;
    else if (m_extraFingers > 0) --m_extraFingers;
    if (m_state != State::Idle) FinishIfAllUp();
}

void TouchGestureRecognizer::Update(const std::uint64_t ns) {
    switch (m_state) {
    case State::Pending:
        if (ns - m_primary.downNs >= m_config.longPressNs) {
            Emit(TouchGesture::LongPress, m_primary);
            m_state = State::LongPressed;
        }
        return;
    case State::SwipeCandidate:
        if (ns - m_primary.downNs > m_config.swipeMaxNs) BecomeDrag(m_primary);
        return;
    case State::TwoFinger:
        if (ns - m_primary.downNs > m_config.twoFingerTapNs)
            m_state = State::Cancelled;
        return;
    default:
        return;
    }
}

std::vector<TouchGestureEvent> TouchGestureRecognizer::Take() {
    return std::exchange(m_events, {});
}

std::optional<std::uint64_t> TouchGestureRecognizer::DragFinger() const {
    if (m_state == State::Dragging && m_primary.down) return m_primary.id;
    return std::nullopt;
}

}  // namespace px
