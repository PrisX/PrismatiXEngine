#include "Engine/Platform/Input.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace px {

Input::Input() {
    BindKey(InputAction::NavigateUp,SDL_SCANCODE_UP);
    BindKey(InputAction::NavigateDown,SDL_SCANCODE_DOWN);
    BindKey(InputAction::NavigateLeft,SDL_SCANCODE_LEFT);
    BindKey(InputAction::NavigateRight,SDL_SCANCODE_RIGHT);
    BindKey(InputAction::Accept,SDL_SCANCODE_RETURN);
    BindKey(InputAction::Accept,SDL_SCANCODE_SPACE);
    BindKey(InputAction::Cancel,SDL_SCANCODE_ESCAPE);
    BindKey(InputAction::FocusNext,SDL_SCANCODE_TAB);
    BindKey(InputAction::Advance,SDL_SCANCODE_RETURN);
    BindKey(InputAction::Advance,SDL_SCANCODE_SPACE);
    BindKey(InputAction::Menu,SDL_SCANCODE_ESCAPE);
    BindKey(InputAction::Backlog,SDL_SCANCODE_B);
    BindKey(InputAction::ToggleAuto,SDL_SCANCODE_A);
    BindKey(InputAction::ToggleSkip,SDL_SCANCODE_S);
    BindKey(InputAction::ToggleUi,SDL_SCANCODE_H);
    BindKey(InputAction::QuickSave,SDL_SCANCODE_F5);
    BindKey(InputAction::QuickLoad,SDL_SCANCODE_F9);
    BindKey(InputAction::Rollback,SDL_SCANCODE_PAGEUP);
    BindKey(InputAction::Pause,SDL_SCANCODE_P);
    BindGamepadButton(InputAction::NavigateUp,SDL_GAMEPAD_BUTTON_DPAD_UP);
    BindGamepadButton(InputAction::NavigateDown,SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    BindGamepadButton(InputAction::NavigateLeft,SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    BindGamepadButton(InputAction::NavigateRight,SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    BindGamepadButton(InputAction::Accept,SDL_GAMEPAD_BUTTON_SOUTH);
    BindGamepadButton(InputAction::Cancel,SDL_GAMEPAD_BUTTON_EAST);
    BindGamepadButton(InputAction::FocusNext,
                      SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    BindGamepadButton(InputAction::FocusPrevious,
                      SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    BindGamepadButton(InputAction::Advance,SDL_GAMEPAD_BUTTON_SOUTH);
    BindGamepadButton(InputAction::Menu,SDL_GAMEPAD_BUTTON_EAST);
    BindGamepadButton(InputAction::Backlog,SDL_GAMEPAD_BUTTON_NORTH);
    BindGamepadButton(InputAction::ToggleAuto,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    BindGamepadButton(InputAction::ToggleSkip,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    BindGamepadButton(InputAction::Pause,SDL_GAMEPAD_BUTTON_START);
}

void Input::ClearBindings(const InputAction action) {
    m_keyBindings[ActionIndex(action)].clear();
    m_gamepadBindings[ActionIndex(action)].clear();
}

void Input::BindKey(const InputAction action,const int scancode) {
    if(action==InputAction::Count||scancode<0)return;
    m_keyBindings[ActionIndex(action)].insert(scancode);
}

void Input::BindGamepadButton(const InputAction action,const int button) {
    if(action==InputAction::Count||button<0)return;
    m_gamepadBindings[ActionIndex(action)].insert(button);
}

bool Input::ActionPressed(const InputAction action) const {
    if(action==InputAction::Count)return false;
    if(m_injectedActions.contains(action))return true;
    if ((action == InputAction::Accept || action == InputAction::Advance) &&
        m_leftClick)
        return true;
    if ((action == InputAction::Cancel || action == InputAction::Menu) &&
        m_rightClick)
        return true;
    const auto& keys=m_keyBindings[ActionIndex(action)];
    if(std::ranges::any_of(keys,[this](const int key){
           return m_keysPressed.contains(key);
       }))return true;
    const auto& buttons=m_gamepadBindings[ActionIndex(action)];
    return std::ranges::any_of(buttons,[this](const int button){
        return m_gamepadButtonsPressed.contains(button);
    });
}

bool Input::ActionDown(const InputAction action) const {
    if(action==InputAction::Count)return false;
    if(m_injectedActions.contains(action))return true;
    if ((action == InputAction::Accept || action == InputAction::Advance) &&
        m_leftDown)
        return true;
    if ((action == InputAction::Cancel || action == InputAction::Menu) &&
        m_rightDown)
        return true;
    const auto& keys=m_keyBindings[ActionIndex(action)];
    if(std::ranges::any_of(keys,[this](const int key){
           return m_keysDown.contains(key);
       }))return true;
    const auto& buttons=m_gamepadBindings[ActionIndex(action)];
    return std::ranges::any_of(buttons,[this](const int button){
        return m_gamepadButtonsDown.contains(button);
    });
}

bool Input::ActionPressedWithoutKeyboard(const InputAction action) const {
    if(action==InputAction::Count)return false;
    if(m_injectedActions.contains(action))return true;
    if ((action == InputAction::Accept || action == InputAction::Advance) &&
        m_leftClick)
        return true;
    if ((action == InputAction::Cancel || action == InputAction::Menu) &&
        m_rightClick)
        return true;
    const auto& buttons=m_gamepadBindings[ActionIndex(action)];
    return std::ranges::any_of(buttons,[this](const int button){
        return m_gamepadButtonsPressed.contains(button);
    });
}

void Input::InjectAction(const InputAction action) {
    if(action!=InputAction::Count)m_injectedActions.insert(action);
}

void Input::InjectKeyPress(const int scancode,const bool held) {
    if(scancode<0)return;
    m_keysPressed.insert(scancode);
    if(held)m_keysDown.insert(scancode);
}

void Input::NewFrame() {
    m_leftClick = false;
    m_leftReleased = false;
    m_rightClick = false;
    m_wheelY = 0.0f;
    m_keysPressed.clear();
    m_gamepadButtonsPressed.clear();
    m_injectedActions.clear();
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

        case SDL_EVENT_FINGER_DOWN:
            if(!m_primaryFinger){
                m_primaryFinger=static_cast<std::uint64_t>(
                    event.tfinger.fingerID);
                int width=1,height=1;
                if(auto* window=SDL_GetWindowFromID(event.tfinger.windowID))
                    (void)SDL_GetWindowSize(window,&width,&height);
                m_mouseX=event.tfinger.x*static_cast<float>(width);
                m_mouseY=event.tfinger.y*static_cast<float>(height);
                m_leftDown=true;
                m_leftClick=true;
            }
            break;

        case SDL_EVENT_FINGER_MOTION:
            if(m_primaryFinger==static_cast<std::uint64_t>(
                   event.tfinger.fingerID)){
                int width=1,height=1;
                if(auto* window=SDL_GetWindowFromID(event.tfinger.windowID))
                    (void)SDL_GetWindowSize(window,&width,&height);
                m_mouseX=event.tfinger.x*static_cast<float>(width);
                m_mouseY=event.tfinger.y*static_cast<float>(height);
            }
            break;

        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            if(m_primaryFinger==static_cast<std::uint64_t>(
                   event.tfinger.fingerID)){
                int width=1,height=1;
                if(auto* window=SDL_GetWindowFromID(event.tfinger.windowID))
                    (void)SDL_GetWindowSize(window,&width,&height);
                m_mouseX=event.tfinger.x*static_cast<float>(width);
                m_mouseY=event.tfinger.y*static_cast<float>(height);
                m_leftReleased=m_leftDown;
                m_leftDown=false;
                m_primaryFinger.reset();
            }
            break;

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            const int button=static_cast<int>(event.gbutton.button);
            if(!m_gamepadButtonsDown.contains(button))
                m_gamepadButtonsPressed.insert(button);
            m_gamepadButtonsDown.insert(button);
            break;
        }

        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            m_gamepadButtonsDown.erase(static_cast<int>(event.gbutton.button));
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
