#include "Engine/Platform/Input.h"

#include <SDL3/SDL.h>

namespace px {

void Input::NewFrame() {
    m_leftClick = false;
    m_leftReleased = false;
    m_rightClick = false;
    m_wheelY = 0.0f;
    m_keysPressed.clear();
    m_textInput.clear();
}

void Input::Process(const SDL_Event& event) {
    switch (event.type) {
        case SDL_EVENT_QUIT:
            m_quit = true;
            break;

        case SDL_EVENT_MOUSE_MOTION:
            m_mouseX = event.motion.x;
            m_mouseY = event.motion.y;
            break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.button == SDL_BUTTON_LEFT) {
                m_leftDown = true;
                m_leftClick = true;
            } else if (event.button.button == SDL_BUTTON_RIGHT) {
                m_rightDown = true;
                m_rightClick = true;
            }
            break;

        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_LEFT) {
                m_leftDown = false;
                m_leftReleased = true;
            } else if (event.button.button == SDL_BUTTON_RIGHT) {
                m_rightDown = false;
            }
            break;

        case SDL_EVENT_MOUSE_WHEEL:
            m_wheelY += event.wheel.y;
            break;

        case SDL_EVENT_KEY_DOWN:
            if (!event.key.repeat) {
                m_keysPressed.insert(static_cast<int>(event.key.scancode));
            }
            m_keysDown.insert(static_cast<int>(event.key.scancode));
            break;

        case SDL_EVENT_KEY_UP:
            m_keysDown.erase(static_cast<int>(event.key.scancode));
            break;

        case SDL_EVENT_TEXT_INPUT:
            m_textInput += event.text.text;
            break;

        default:
            break;
    }
}

}
