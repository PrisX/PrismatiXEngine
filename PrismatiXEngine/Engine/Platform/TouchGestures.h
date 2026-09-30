#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace px {

// How the Player treats touch input.
enum class TouchProfile : std::uint8_t {
    // Touch gestures are recognized. platform.touch follows the device the
    // player used last (finger versus real mouse).
    Auto,
    // Same gestures, and platform.touch is always true: lets authors try
    // touch layouts on desktop.
    On,
    // The previous behaviour: the first finger acts as a mouse pointer and
    // platform.touch is always false.
    Off,
};

[[nodiscard]] std::optional<TouchProfile> ParseTouchProfile(std::string_view text);

enum class TouchGesture : std::uint8_t {
    // A short touch that stayed in place: advance / activate.
    Tap,
    // A touch held in place: hide or show the message window.
    LongPress,
    // Two fingers tapped together: open the game menu.
    TwoFingerTap,
    // A quick upward flick: open the backlog.
    SwipeUp,
    // A touch that moved without being a swipe. The finger now drives the
    // pointer (sliders, scrollbars) until it lifts.
    DragStart,
};

struct TouchGestureEvent {
    TouchGesture gesture = TouchGesture::Tap;
    // Where the gesture began, in the coordinates the caller supplied.
    float x = 0.0f;
    float y = 0.0f;
    friend bool operator==(const TouchGestureEvent&, const TouchGestureEvent&) = default;
};

struct TouchGestureConfig {
    std::uint64_t longPressNs = 500'000'000;
    // Both fingers of a two-finger tap must lift within this time.
    std::uint64_t twoFingerTapNs = 400'000'000;
    // An upward move is a swipe only if it finishes within this time;
    // otherwise it becomes a drag.
    std::uint64_t swipeMaxNs = 600'000'000;
    // Movement below this distance still counts as "in place".
    float slop = 24.0f;
    float swipeMinDistance = 96.0f;
};

// Classifies raw finger events into gestures. Knows nothing about SDL, so
// the rules are unit tested with synthetic timelines. Times are monotonic
// nanoseconds; positions are in any consistent unit (the Player passes
// logical render coordinates).
class TouchGestureRecognizer {
public:
    explicit TouchGestureRecognizer(TouchGestureConfig config = {})
        : m_config(config) {}

    void Down(std::uint64_t finger, float x, float y, std::uint64_t ns);
    void Move(std::uint64_t finger, float x, float y, std::uint64_t ns);
    void Up(std::uint64_t finger, float x, float y, std::uint64_t ns);
    // The OS took the touch away (e.g. a system gesture): no gesture fires.
    void Cancel(std::uint64_t finger);
    // Fires time-based gestures (long press) and turns a slow upward move
    // into a drag. Call once per frame.
    void Update(std::uint64_t ns);

    [[nodiscard]] std::vector<TouchGestureEvent> Take();
    // The finger currently driving the pointer after a DragStart.
    [[nodiscard]] std::optional<std::uint64_t> DragFinger() const;
    [[nodiscard]] bool Idle() const { return m_state == State::Idle; }

private:
    enum class State : std::uint8_t {
        Idle,
        Pending,         // one finger down, not yet classified
        SwipeCandidate,  // one finger moving upward quickly
        Dragging,
        LongPressed,     // long press fired; wait for the finger to lift
        TwoFinger,       // two fingers down, both still in place
        Cancelled,       // no gesture; wait for every finger to lift
    };
    struct Finger {
        std::uint64_t id = 0;
        float startX = 0.0f;
        float startY = 0.0f;
        float x = 0.0f;
        float y = 0.0f;
        std::uint64_t downNs = 0;
        bool down = false;
        bool moved = false;
    };

    [[nodiscard]] Finger* Find(std::uint64_t id);
    [[nodiscard]] bool AnyDown() const;
    void Emit(TouchGesture gesture, const Finger& finger);
    void BecomeDrag(Finger& finger);
    void FinishIfAllUp();

    TouchGestureConfig m_config;
    State m_state = State::Idle;
    Finger m_primary;
    Finger m_secondary;
    std::uint64_t m_extraFingers = 0;
    std::vector<TouchGestureEvent> m_events;
};

}  // namespace px
