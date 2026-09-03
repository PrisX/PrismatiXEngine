#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <string>

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

    [[nodiscard]] float MouseX() const { return m_mouseX; }
    [[nodiscard]] float MouseY() const { return m_mouseY; }
    [[nodiscard]] float WheelY() const { return m_wheelY; }
    [[nodiscard]] const std::string& TextInput() const { return m_textInput; }
    [[nodiscard]] const std::set<int>& PressedKeys() const { return m_keysPressed; }

    [[nodiscard]] bool LeftClick() const { return m_leftClick; }
    [[nodiscard]] bool LeftReleased() const { return m_leftReleased; }
    [[nodiscard]] bool RightClick() const { return m_rightClick; }
    [[nodiscard]] bool LeftDown() const { return m_leftDown; }
    [[nodiscard]] bool RightDown() const { return m_rightDown; }

    [[nodiscard]] bool QuitRequested() const { return m_quit; }

    [[nodiscard]] bool KeyPressed(int scancode) const {
        return m_keysPressed.count(scancode) != 0;
    }
    [[nodiscard]] bool KeyDown(int scancode) const {
        return m_keysDown.count(scancode) != 0;
    }

    void InjectFrame(float mouseX, float mouseY, bool leftClick) {
        const bool wasLeftDown = m_leftDown;
        m_keysPressed.clear();
        m_keysDown.clear();
        m_textInput.clear();
        m_mouseX = mouseX;
        m_mouseY = mouseY;
        m_leftClick = leftClick;
        m_leftReleased = wasLeftDown && !leftClick;
        m_leftDown = leftClick;
        m_rightClick = false;
        m_wheelY = 0.0f;
        m_gamepadButtonsPressed.clear();
        m_injectedActions.clear();
    }

private:
    static constexpr std::size_t kActionCount =
        static_cast<std::size_t>(InputAction::Count);
    static constexpr std::size_t ActionIndex(InputAction action) {
        return static_cast<std::size_t>(action);
    }
    std::set<int> m_keysPressed;
    std::set<int> m_keysDown;
    std::set<int> m_gamepadButtonsPressed;
    std::set<int> m_gamepadButtonsDown;
    std::array<std::set<int>,kActionCount> m_keyBindings;
    std::array<std::set<int>,kActionCount> m_gamepadBindings;
    std::set<InputAction> m_injectedActions;
    float m_mouseX = -1000.0f;
    float m_mouseY = -1000.0f;
    float m_wheelY = 0.0f;
    bool m_leftDown = false;
    bool m_rightDown = false;
    bool m_leftClick = false;
    bool m_leftReleased = false;
    bool m_rightClick = false;
    bool m_quit = false;
    std::optional<std::uint64_t> m_primaryFinger;
    std::string m_textInput;
};

}
