#pragma once

#include <set>

union SDL_Event;

namespace px {

class Input {
public:
    void NewFrame();
    void Process(const SDL_Event& event);

    [[nodiscard]] float MouseX() const { return m_mouseX; }
    [[nodiscard]] float MouseY() const { return m_mouseY; }
    [[nodiscard]] float WheelY() const { return m_wheelY; }

    [[nodiscard]] bool LeftClick() const { return m_leftClick; }
    [[nodiscard]] bool RightClick() const { return m_rightClick; }
    [[nodiscard]] bool LeftDown() const { return m_leftDown; }
    [[nodiscard]] bool RightDown() const { return m_rightDown; }

    [[nodiscard]] bool QuitRequested() const { return m_quit; }

    [[nodiscard]] bool KeyPressed(int scancode) const {
        return m_keysPressed.count(scancode) != 0;
    }

    void InjectFrame(float mouseX, float mouseY, bool leftClick) {
        m_keysPressed.clear();
        m_mouseX = mouseX;
        m_mouseY = mouseY;
        m_leftClick = leftClick;
        m_leftDown = leftClick;
        m_rightClick = false;
        m_wheelY = 0.0f;
    }

private:
    std::set<int> m_keysPressed;
    float m_mouseX = -1000.0f;
    float m_mouseY = -1000.0f;
    float m_wheelY = 0.0f;
    bool m_leftDown = false;
    bool m_rightDown = false;
    bool m_leftClick = false;
    bool m_rightClick = false;
    bool m_quit = false;
};

}
