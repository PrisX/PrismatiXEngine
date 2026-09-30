#include "Engine/Platform/Input.h"
#include "Engine/Platform/TouchGestures.h"
#include "Tests/TestSupport/TestHarness.h"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace {

using px::TouchGesture;
using px::TouchGestureEvent;
using px::TouchGestureRecognizer;

constexpr std::uint64_t kMs = 1'000'000;

std::string Describe(const std::vector<TouchGestureEvent>& events) {
    std::string text;
    for (const auto& event : events) {
        if (!text.empty()) text += ',';
        switch (event.gesture) {
        case TouchGesture::Tap: text += "tap"; break;
        case TouchGesture::LongPress: text += "long-press"; break;
        case TouchGesture::TwoFingerTap: text += "two-finger-tap"; break;
        case TouchGesture::SwipeUp: text += "swipe-up"; break;
        case TouchGesture::DragStart: text += "drag"; break;
        }
    }
    return text;
}

SDL_Event Finger(const SDL_EventType type, const SDL_FingerID id, const float x,
                 const float y, const std::uint64_t ms) {
    SDL_Event event{};
    event.type = type;
    event.tfinger.fingerID = id;
    event.tfinger.x = x;
    event.tfinger.y = y;
    event.tfinger.timestamp = ms * kMs;
    return event;
}

SDL_Event MouseButton(const SDL_EventType type, const SDL_MouseID which) {
    SDL_Event event{};
    event.type = type;
    event.button.which = which;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = 10.0f;
    event.button.y = 20.0f;
    return event;
}

// Fingers map to a 1000x500 logical screen, like the Runtime's mapper.
px::Input TouchInput(const px::TouchProfile profile = px::TouchProfile::Auto) {
    px::Input input;
    input.SetTouchProfile(profile);
    input.SetTouchCoordinateMapper([](const float nx, const float ny, float& x, float& y) {
        x = nx * 1000.0f;
        y = ny * 500.0f;
        return true;
    });
    input.NewFrame();
    return input;
}

}  // namespace

int main() {
    px::test::Suite suite("TouchGestures");

    suite.Run("ParseProfile", [&] {
        suite.Expect(px::ParseTouchProfile("auto") == px::TouchProfile::Auto &&
                         px::ParseTouchProfile("on") == px::TouchProfile::On &&
                         px::ParseTouchProfile("off") == px::TouchProfile::Off &&
                         !px::ParseTouchProfile("touch") && !px::ParseTouchProfile(""),
                     "the three spellings parse; anything else is rejected");
    });

    suite.Run("TapInPlace", [&] {
        TouchGestureRecognizer recognizer;
        recognizer.Down(1, 100, 100, 0);
        recognizer.Move(1, 110, 108, 50 * kMs);
        recognizer.Update(80 * kMs);
        suite.Equal("", Describe(recognizer.Take()), "nothing fires while the finger is down");
        recognizer.Up(1, 112, 110, 120 * kMs);
        const auto events = recognizer.Take();
        suite.Equal("tap", Describe(events), "jitter inside the slop is still a tap");
        suite.Expect(!events.empty() && events[0].x == 100 && events[0].y == 100,
                     "a tap reports where it began");
        suite.Expect(recognizer.Idle(), "idle after the finger lifts");
    });

    suite.Run("LongPressFiresOnceWhileHeld", [&] {
        TouchGestureRecognizer recognizer;
        recognizer.Down(1, 100, 100, 0);
        recognizer.Update(499 * kMs);
        suite.Equal("", Describe(recognizer.Take()), "not before the threshold");
        recognizer.Update(500 * kMs);
        recognizer.Update(900 * kMs);
        suite.Equal("long-press", Describe(recognizer.Take()), "fires once at 0.5 s");
        recognizer.Up(1, 100, 100, 1000 * kMs);
        suite.Equal("", Describe(recognizer.Take()), "lifting afterwards is not a tap");
        suite.Expect(recognizer.Idle(), "idle again");
    });

    suite.Run("SwipeUpNeedsDistanceSpeedAndDirection", [&] {
        TouchGestureRecognizer fast;
        fast.Down(1, 500, 400, 0);
        fast.Move(1, 505, 330, 60 * kMs);
        fast.Move(1, 510, 250, 120 * kMs);
        fast.Up(1, 512, 240, 150 * kMs);
        suite.Equal("swipe-up", Describe(fast.Take()), "a quick upward flick");

        TouchGestureRecognizer shortFlick;
        shortFlick.Down(1, 500, 400, 0);
        shortFlick.Move(1, 500, 350, 50 * kMs);
        shortFlick.Up(1, 500, 340, 80 * kMs);
        suite.Equal("", Describe(shortFlick.Take()), "too short for a swipe and not a tap");

        TouchGestureRecognizer slow;
        slow.Down(1, 500, 400, 0);
        slow.Move(1, 500, 350, 100 * kMs);
        slow.Update(601 * kMs);
        suite.Equal("drag", Describe(slow.Take()), "a slow upward move becomes a drag");
        suite.Expect(slow.DragFinger() == std::uint64_t{1}, "the finger drives the pointer");
        slow.Up(1, 500, 200, 800 * kMs);
        suite.Equal("", Describe(slow.Take()), "a drag never turns into a swipe");

        TouchGestureRecognizer sideways;
        sideways.Down(1, 500, 400, 0);
        sideways.Move(1, 600, 390, 40 * kMs);
        suite.Equal("drag", Describe(sideways.Take()), "a sideways move drags at once (sliders)");
    });

    suite.Run("TwoFingerTap", [&] {
        TouchGestureRecognizer recognizer;
        recognizer.Down(1, 100, 100, 0);
        recognizer.Down(2, 300, 100, 30 * kMs);
        recognizer.Up(1, 100, 100, 150 * kMs);
        suite.Equal("", Describe(recognizer.Take()), "waits for both fingers");
        recognizer.Up(2, 300, 100, 180 * kMs);
        suite.Equal("two-finger-tap", Describe(recognizer.Take()), "both lifted in time");

        TouchGestureRecognizer slow;
        slow.Down(1, 100, 100, 0);
        slow.Down(2, 300, 100, 30 * kMs);
        slow.Update(450 * kMs);
        slow.Up(1, 100, 100, 460 * kMs);
        slow.Up(2, 300, 100, 470 * kMs);
        suite.Equal("", Describe(slow.Take()), "held too long: no gesture, and no long press");

        TouchGestureRecognizer pinch;
        pinch.Down(1, 100, 100, 0);
        pinch.Down(2, 300, 100, 30 * kMs);
        pinch.Move(2, 380, 100, 60 * kMs);
        pinch.Up(1, 100, 100, 100 * kMs);
        pinch.Up(2, 380, 100, 110 * kMs);
        suite.Equal("", Describe(pinch.Take()), "a moving finger cancels the tap");

        TouchGestureRecognizer three;
        three.Down(1, 100, 100, 0);
        three.Down(2, 300, 100, 10 * kMs);
        three.Down(3, 500, 100, 20 * kMs);
        three.Up(1, 100, 100, 50 * kMs);
        three.Up(2, 300, 100, 60 * kMs);
        suite.Expect(!three.Idle(), "still waiting for the third finger");
        three.Up(3, 500, 100, 70 * kMs);
        suite.Expect(Describe(three.Take()).empty() && three.Idle(),
                     "three fingers are not a two-finger tap");
    });

    suite.Run("CancelledTouchFiresNothing", [&] {
        TouchGestureRecognizer recognizer;
        recognizer.Down(1, 100, 100, 0);
        recognizer.Cancel(1);
        recognizer.Update(900 * kMs);
        suite.Expect(Describe(recognizer.Take()).empty() && recognizer.Idle(),
                     "a system gesture that steals the touch is ignored");
    });

    suite.Run("InputTapIsOneCompleteClick", [&] {
        auto input = TouchInput();
        input.Process(Finger(SDL_EVENT_FINGER_DOWN, 7, 0.5f, 0.5f, 0));
        suite.Expect(!input.LeftClick() && !input.ActionPressed(px::InputAction::Advance),
                     "a finger going down does not advance yet");
        suite.Expect(input.MouseX() == 500.0f && input.MouseY() == 250.0f,
                     "hover follows the finger through the coordinate mapper");
        input.NewFrame();
        input.Process(Finger(SDL_EVENT_FINGER_UP, 7, 0.5f, 0.5f, 100));
        suite.Expect(input.LeftClick() && input.LeftReleased() && !input.LeftDown() &&
                         input.ActionPressed(px::InputAction::Advance) &&
                         input.ActionPressed(px::InputAction::Accept),
                     "tap = press and release in one frame with the click's actions");
    });

    suite.Run("InputGesturesFireTheirActions", [&] {
        auto longPress = TouchInput();
        longPress.Process(Finger(SDL_EVENT_FINGER_DOWN, 1, 0.5f, 0.5f, 0));
        longPress.NewFrame();
        longPress.UpdateTouch(600 * kMs);
        suite.Expect(longPress.ActionPressed(px::InputAction::ToggleUi) &&
                         !longPress.LeftClick() && !longPress.RightClick(),
                     "long press = toggle-ui (hide the message window)");

        auto twoFinger = TouchInput();
        twoFinger.Process(Finger(SDL_EVENT_FINGER_DOWN, 1, 0.2f, 0.5f, 0));
        twoFinger.Process(Finger(SDL_EVENT_FINGER_DOWN, 2, 0.6f, 0.5f, 20));
        twoFinger.Process(Finger(SDL_EVENT_FINGER_UP, 1, 0.2f, 0.5f, 120));
        twoFinger.Process(Finger(SDL_EVENT_FINGER_UP, 2, 0.6f, 0.5f, 130));
        suite.Expect(twoFinger.ActionPressed(px::InputAction::Menu) &&
                         !twoFinger.RightClick() && !twoFinger.LeftClick(),
                     "two-finger tap = menu, without also clicking");

        auto swipe = TouchInput();
        swipe.Process(Finger(SDL_EVENT_FINGER_DOWN, 1, 0.5f, 0.8f, 0));
        swipe.Process(Finger(SDL_EVENT_FINGER_MOTION, 1, 0.5f, 0.6f, 60));
        swipe.Process(Finger(SDL_EVENT_FINGER_UP, 1, 0.5f, 0.4f, 140));
        suite.Expect(swipe.ActionPressed(px::InputAction::Backlog) && !swipe.LeftClick(),
                     "swipe up = backlog, without advancing the text");
    });

    suite.Run("InputDragDrivesThePointer", [&] {
        auto input = TouchInput();
        input.Process(Finger(SDL_EVENT_FINGER_DOWN, 1, 0.1f, 0.5f, 0));
        input.NewFrame();
        input.Process(Finger(SDL_EVENT_FINGER_MOTION, 1, 0.3f, 0.5f, 30));
        suite.Expect(input.LeftClick() && input.LeftDown() && input.MouseX() == 300.0f,
                     "a sideways drag presses at its start and follows the finger");
        input.NewFrame();
        input.Process(Finger(SDL_EVENT_FINGER_MOTION, 1, 0.4f, 0.5f, 60));
        suite.Expect(input.LeftDown() && !input.LeftClick() && input.MouseX() == 400.0f,
                     "the pointer stays held while dragging");
        input.NewFrame();
        input.Process(Finger(SDL_EVENT_FINGER_UP, 1, 0.45f, 0.5f, 90));
        suite.Expect(input.LeftReleased() && !input.LeftDown() && input.MouseX() == 450.0f,
                     "lifting releases where the finger left");
    });

    suite.Run("OffKeepsTheFirstFingerAsAPointer", [&] {
        auto input = TouchInput(px::TouchProfile::Off);
        input.Process(Finger(SDL_EVENT_FINGER_DOWN, 1, 0.5f, 0.5f, 0));
        suite.Expect(input.LeftClick() && input.LeftDown() &&
                         input.ActionPressed(px::InputAction::Advance),
                     "off presses immediately, as before");
        input.NewFrame();
        input.UpdateTouch(900 * kMs);
        suite.Expect(!input.ActionPressed(px::InputAction::ToggleUi),
                     "off recognizes no gestures");
    });

    suite.Run("MouseIsUnchangedAndTouchCopiesAreDropped", [&] {
        for (const auto profile : {px::TouchProfile::Auto, px::TouchProfile::On,
                                   px::TouchProfile::Off}) {
            auto input = TouchInput(profile);
            input.Process(MouseButton(SDL_EVENT_MOUSE_BUTTON_DOWN, 1));
            suite.Expect(input.LeftClick() && input.LeftDown() &&
                             input.ActionPressed(px::InputAction::Advance) &&
                             input.MouseX() == 10.0f,
                         "a real mouse press behaves the same under every profile");
            input.NewFrame();
            input.Process(MouseButton(SDL_EVENT_MOUSE_BUTTON_UP, 1));
            suite.Expect(input.LeftReleased(), "and so does its release");

            auto synthetic = TouchInput(profile);
            synthetic.Process(MouseButton(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_TOUCH_MOUSEID));
            suite.Expect(!synthetic.LeftClick() && synthetic.FrameEvents().empty(),
                         "SDL's mouse copy of a touch is ignored");
        }
    });

    suite.Run("TouchActiveFollowsProfileAndDevice", [&] {
        auto input = TouchInput();
        suite.Expect(input.TakeTouchActiveChange() == false,
                     "the first query reports the initial value");
        suite.Expect(!input.TakeTouchActiveChange(), "unchanged values are not repeated");
        input.Process(Finger(SDL_EVENT_FINGER_DOWN, 1, 0.5f, 0.5f, 0));
        suite.Expect(input.TouchActive() && input.TakeTouchActiveChange() == true,
                     "a finger switches Auto to touch");
        SDL_Event motion{};
        motion.type = SDL_EVENT_MOUSE_MOTION;
        motion.motion.which = 1;
        input.Process(motion);
        suite.Expect(!input.TouchActive() && input.TakeTouchActiveChange() == false,
                     "a real mouse switches back");
        motion.motion.which = SDL_TOUCH_MOUSEID;
        input.Process(Finger(SDL_EVENT_FINGER_UP, 1, 0.5f, 0.5f, 50));
        input.Process(motion);
        suite.Expect(input.TouchActive(), "SDL's mouse copy of a touch does not");

        auto forced = TouchInput(px::TouchProfile::On);
        suite.Expect(forced.TouchActive() && forced.TakeTouchActiveChange() == true,
                     "on forces touch layouts on desktop");
        auto off = TouchInput(px::TouchProfile::Off);
        off.Process(Finger(SDL_EVENT_FINGER_DOWN, 1, 0.5f, 0.5f, 0));
        suite.Expect(!off.TouchActive(), "off never reports touch");
    });

    return suite.Finish();
}
