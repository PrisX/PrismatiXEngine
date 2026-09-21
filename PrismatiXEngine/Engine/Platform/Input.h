#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <vector>

union SDL_Event;

namespace px {

enum class InputAction : std::uint8_t {
    NavigateUp,
    NavigateDown,
    NavigateLeft,
    NavigateRight,
    Accept,
    Cancel,
    FocusNext,
    FocusPrevious,
    Advance,
    Menu,
    Backlog,
    ToggleAuto,
    ToggleSkip,
    ToggleUi,
    QuickSave,
    QuickLoad,
    Rollback,
    Pause,
    Count,
};

// Device-neutral events are the public per-frame input primitive.  Platform
// identifiers (SDL scancodes, mouse button numbers, and gamepad button
// numbers) deliberately stay in the binding layer below this type.
enum class InputEventType : std::uint8_t {
    PointerDown,
    PointerUp,
    PointerMove,
    PointerWheel,
    TextInput,
    Text = TextInput,
    Action,
};

enum class PointerButton : std::uint8_t {
    None,
    Primary,
    Secondary,
    Middle,
};

struct InputEvent {
    std::uint64_t id = 0;
    std::uint64_t frame = 0;
    // Logical action events reference the pointer event that produced them.
    // Zero means the action was injected or originated from a non-pointer
    // binding whose platform identity intentionally remains private.
    std::uint64_t sourceId = 0;
    InputEventType type = InputEventType::PointerMove;
    std::uint64_t pointerId = 0;
    PointerButton button = PointerButton::None;
    float x = 0.0f;
    float y = 0.0f;
    float deltaX = 0.0f;
    float deltaY = 0.0f;
    std::string text;
    std::string action;
};

class Input {
public:
    Input();
    void NewFrame();
    void Process(const SDL_Event& event);

    void ClearBindings(InputAction action);
    void BindKey(InputAction action, int scancode);
    void BindGamepadButton(InputAction action, int button);
    [[nodiscard]] bool ActionPressed(InputAction action) const;
    [[nodiscard]] bool ActionDown(InputAction action) const;
    [[nodiscard]] bool ActionPressedWithoutKeyboard(InputAction action) const;
    // Deterministic automation seam used by Player E2E and input conformance.
    void InjectAction(InputAction action);
    void InjectKeyPress(int scancode, bool held = false);

    // String actions are intentionally open-ended. Built-in action names use
    // the same kebab-case spelling as the scripting surface ("accept",
    // "navigate-up", ...). Registering a built-in is harmless and idempotent.
    [[nodiscard]] bool RegisterAction(std::string action);
    [[nodiscard]] bool HasAction(std::string_view action) const;
    [[nodiscard]] bool UnregisterAction(std::string_view action);
    [[nodiscard]] bool ClearBindings(std::string_view action);
    [[nodiscard]] bool BindKey(std::string_view action, int scancode);
    [[nodiscard]] bool BindGamepadButton(std::string_view action, int button);
    [[nodiscard]] bool ActionPressed(std::string_view action) const;
    [[nodiscard]] bool ActionDown(std::string_view action) const;
    [[nodiscard]] bool InjectAction(std::string_view action);
    [[nodiscard]] static std::string_view ActionName(InputAction action);

    // The span is immutable and remains valid until Process(), NewFrame(), or
    // Inject*() is called. Event ids are process-lifetime monotonic and never
    // reused, including across frame boundaries.
    [[nodiscard]] std::span<const InputEvent> FrameEvents() const {
        return m_events;
    }
    [[nodiscard]] std::uint64_t FrameSequence() const { return m_frameSequence; }

    // Consumption controls propagation between extension/UI listeners.
    // Default suppression is separate: a listener may continue propagation
    // while asking the host not to perform its stock action.
    [[nodiscard]] bool ConsumeEvent(std::uint64_t eventId);
    [[nodiscard]] bool SuppressDefault(std::uint64_t eventId);
    [[nodiscard]] bool EventConsumed(std::uint64_t eventId) const;
    [[nodiscard]] bool DefaultSuppressed(std::uint64_t eventId) const;

    [[nodiscard]] float MouseX() const { return m_mouseX; }
    [[nodiscard]] float MouseY() const { return m_mouseY; }
    [[nodiscard]] float WheelY() const {
        return m_suppressWheel ? 0.0f : m_wheelY;
    }
    [[nodiscard]] const std::string& TextInput() const {
        if (m_suppressText) {
            static const std::string empty;
            return empty;
        }
        return m_textInput;
    }
    [[nodiscard]] const std::set<int>& PressedKeys() const { return m_keysPressed; }

    [[nodiscard]] bool LeftClick() const {
        return m_leftClick && !m_suppressLeftPointer;
    }
    [[nodiscard]] bool LeftReleased() const {
        return m_leftReleased && !m_suppressLeftPointer;
    }
    [[nodiscard]] bool RightClick() const {
        return m_rightClick && !m_suppressRightPointer;
    }
    [[nodiscard]] bool LeftDown() const {
        return m_leftDown && !m_suppressLeftPointer;
    }
    [[nodiscard]] bool RightDown() const {
        return m_rightDown && !m_suppressRightPointer;
    }

    [[nodiscard]] bool QuitRequested() const { return m_quit; }

    [[nodiscard]] bool KeyPressed(int scancode) const {
        return m_keysPressed.count(scancode) != 0 &&
               !m_suppressedKeys.contains(scancode);
    }
    [[nodiscard]] bool KeyDown(int scancode) const {
        return m_keysDown.count(scancode) != 0 &&
               !m_suppressedKeys.contains(scancode);
    }

    void InjectFrame(float mouseX, float mouseY, bool leftClick);

private:
    static constexpr std::size_t kActionCount =
        static_cast<std::size_t>(InputAction::Count);
    static constexpr std::size_t ActionIndex(InputAction action) {
        return static_cast<std::size_t>(action);
    }
    struct NamedBindings {
        std::set<int> keys;
        std::set<int> gamepadButtons;
    };
    enum class EventSourceKind : std::uint8_t {
        None,
        Key,
        GamepadButton,
        Pointer,
    };
    struct EventSource {
        EventSourceKind kind = EventSourceKind::None;
        int code = 0;
        PointerButton pointerButton = PointerButton::None;
    };

    [[nodiscard]] bool HasCurrentEvent(std::uint64_t eventId) const;
    [[nodiscard]] bool EventDefaultSuppressed(const InputEvent& event) const;
    [[nodiscard]] std::uint64_t AppendEvent(
        InputEvent event, EventSource source = {});
    void AppendActionEventsForKey(int scancode);
    void AppendActionEventsForGamepadButton(int button);
    void AppendPointerDefaultActions(PointerButton button,
                                     std::uint64_t sourceId);
    std::set<int> m_keysPressed;
    std::set<int> m_keysDown;
    std::set<int> m_gamepadButtonsPressed;
    std::set<int> m_gamepadButtonsDown;
    std::array<std::set<int>,kActionCount> m_keyBindings;
    std::array<std::set<int>,kActionCount> m_gamepadBindings;
    std::set<InputAction> m_injectedActions;
    std::map<std::string,NamedBindings,std::less<>> m_namedBindings;
    std::set<std::string,std::less<>> m_injectedNamedActions;
    std::vector<InputEvent> m_events;
    std::unordered_set<std::uint64_t> m_consumedEvents;
    std::unordered_set<std::uint64_t> m_suppressedDefaults;
    std::unordered_map<std::uint64_t,EventSource> m_eventSources;
    std::set<int> m_suppressedKeys;
    std::set<int> m_suppressedGamepadButtons;
    std::uint64_t m_nextEventId = 1;
    std::uint64_t m_frameSequence = 0;
    float m_mouseX = -1000.0f;
    float m_mouseY = -1000.0f;
    float m_wheelY = 0.0f;
    bool m_leftDown = false;
    bool m_rightDown = false;
    bool m_leftClick = false;
    bool m_leftReleased = false;
    bool m_rightClick = false;
    bool m_quit = false;
    bool m_suppressLeftPointer = false;
    bool m_suppressRightPointer = false;
    bool m_suppressWheel = false;
    bool m_suppressText = false;
    std::optional<std::uint64_t> m_primaryFinger;
    std::string m_textInput;
};

}
