#include "Engine/Platform/Input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace px {

namespace {
std::optional<InputAction> BuiltinAction(const std::string_view name) {
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(InputAction::Count); ++index) {
        const auto action = static_cast<InputAction>(index);
        if (Input::ActionName(action) == name) return action;
    }
    return std::nullopt;
}
}

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
    BindKey(InputAction::OpenSave,SDL_SCANCODE_F6);
    (void)SetActionContext(ActionName(InputAction::OpenSave), "game");
    BindKey(InputAction::Rollback,SDL_SCANCODE_PAGEUP);
    BindKey(InputAction::Pause,SDL_SCANCODE_P);
    (void)SetWheelBinding(ActionName(InputAction::Backlog), WheelDirection::Up);
    (void)SetWheelBinding(ActionName(InputAction::Advance), WheelDirection::Down);
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

std::string_view Input::ActionName(const InputAction action) {
    switch (action) {
        case InputAction::NavigateUp: return "navigate-up";
        case InputAction::NavigateDown: return "navigate-down";
        case InputAction::NavigateLeft: return "navigate-left";
        case InputAction::NavigateRight: return "navigate-right";
        case InputAction::Accept: return "accept";
        case InputAction::Cancel: return "cancel";
        case InputAction::FocusNext: return "focus-next";
        case InputAction::FocusPrevious: return "focus-previous";
        case InputAction::Advance: return "advance";
        case InputAction::Menu: return "menu";
        case InputAction::Backlog: return "backlog";
        case InputAction::ToggleAuto: return "toggle-auto";
        case InputAction::ToggleSkip: return "toggle-skip";
        case InputAction::ToggleUi: return "toggle-ui";
        case InputAction::QuickSave: return "quick-save";
        case InputAction::QuickLoad: return "quick-load";
        case InputAction::OpenSave: return "save.open";
        case InputAction::Rollback: return "rollback";
        case InputAction::Pause: return "pause";
        case InputAction::Count: return {};
    }
    return {};
}

void Input::ClearBindings(const InputAction action) {
    if (action == InputAction::Count) return;
    m_keyBindings[ActionIndex(action)].clear();
    m_gamepadBindings[ActionIndex(action)].clear();
    (void)ClearBindings(ActionName(action));
}

void Input::BindKey(const InputAction action,const int scancode) {
    if(action==InputAction::Count || scancode <= SDL_SCANCODE_UNKNOWN ||
       scancode >= SDL_SCANCODE_COUNT) return;
    m_keyBindings[ActionIndex(action)].insert(scancode);
    const std::string_view name = ActionName(action);
    if (!HasAction(name)) (void)RegisterAction(std::string(name));
    (void)BindKey(name, scancode);
}

void Input::BindGamepadButton(const InputAction action,const int button) {
    if(action==InputAction::Count||button<0)return;
    m_gamepadBindings[ActionIndex(action)].insert(button);
    const std::string_view name = ActionName(action);
    if (!HasAction(name)) (void)RegisterAction(std::string(name));
    (void)BindGamepadButton(name, button);
}

bool Input::RegisterAction(std::string action) {
    if (action.empty() || action.find('\0') != std::string::npos) return false;
    const auto [found, inserted] = m_namedBindings.try_emplace(std::move(action));
    if (!inserted) return false;
    if (const auto pending = m_pendingKeyBindings.find(found->first);
        pending != m_pendingKeyBindings.end()) {
        found->second.keys = {pending->second.begin(), pending->second.end()};
        found->second.keyOrder = pending->second;
        if (const auto builtin = BuiltinAction(found->first))
            m_keyBindings[ActionIndex(*builtin)] = found->second.keys;
        m_pendingKeyBindings.erase(pending);
    }
    if (const auto pending = m_pendingWheelBindings.find(found->first);
        pending != m_pendingWheelBindings.end()) {
        found->second.wheelDirection = pending->second.direction;
        found->second.wheelThreshold = pending->second.threshold;
        m_pendingWheelBindings.erase(pending);
    }
    return true;
}

bool Input::HasAction(const std::string_view action) const {
    return m_namedBindings.find(action) != m_namedBindings.end();
}

bool Input::UnregisterAction(const std::string_view action) {
    if (BuiltinAction(action)) return false;
    const std::string name(action);
    m_injectedNamedActions.erase(name);
    m_pendingKeyBindings.erase(name);
    m_pendingWheelBindings.erase(name);
    m_defaultKeyboardBindings.erase(name);
    m_defaultWheelBindings.erase(name);
    return m_namedBindings.erase(name) != 0;
}

bool Input::ClearBindings(const std::string_view action) {
    const auto found = m_namedBindings.find(action);
    if (found == m_namedBindings.end()) return false;
    found->second = {};
    if (const auto builtin = BuiltinAction(action)) {
        m_keyBindings[ActionIndex(*builtin)].clear();
        m_gamepadBindings[ActionIndex(*builtin)].clear();
    }
    return true;
}

bool Input::ClearKeyboardBindings(const std::string_view action) {
    const auto found = m_namedBindings.find(action);
    if (found == m_namedBindings.end()) return false;
    found->second.keys.clear();
    found->second.keyOrder.clear();
    if (const auto builtin = BuiltinAction(action))
        m_keyBindings[ActionIndex(*builtin)].clear();
    return true;
}

bool Input::BindKey(const std::string_view action, const int scancode) {
    const auto found = m_namedBindings.find(action);
    if (found == m_namedBindings.end() ||
        scancode <= SDL_SCANCODE_UNKNOWN ||
        scancode >= SDL_SCANCODE_COUNT) return false;
    if (!found->second.keys.contains(scancode) &&
        found->second.keyOrder.size() >= 32) return false;
    if (found->second.keys.insert(scancode).second)
        found->second.keyOrder.push_back(scancode);
    if (const auto builtin = BuiltinAction(action))
        m_keyBindings[ActionIndex(*builtin)].insert(scancode);
    return true;
}

bool Input::BindGamepadButton(const std::string_view action, const int button) {
    const auto found = m_namedBindings.find(action);
    if (found == m_namedBindings.end() || button < 0) return false;
    found->second.gamepadButtons.insert(button);
    if (const auto builtin = BuiltinAction(action))
        m_gamepadBindings[ActionIndex(*builtin)].insert(button);
    return true;
}

bool Input::SetWheelBinding(const std::string_view action,
                            const WheelDirection direction,
                            const float threshold) {
    const auto found = m_namedBindings.find(action);
    if (found == m_namedBindings.end() || !std::isfinite(threshold) ||
        threshold < 0.0f || threshold > 100.0f ||
        (direction != WheelDirection::None && direction != WheelDirection::Up &&
         direction != WheelDirection::Down) ||
        (direction == WheelDirection::None && threshold != 0.0f)) return false;
    found->second.wheelDirection = direction;
    found->second.wheelThreshold = threshold;
    return true;
}

std::optional<WheelBinding> Input::GetWheelBinding(
    const std::string_view action) const {
    const auto found = m_namedBindings.find(action);
    if (found == m_namedBindings.end()) return std::nullopt;
    return WheelBinding{found->second.wheelDirection,
                        found->second.wheelThreshold};
}

WheelBindingMap Input::CaptureWheelBindings() const {
    WheelBindingMap result = m_pendingWheelBindings;
    for (const auto& [action, binding] : m_namedBindings)
        result[action] = {binding.wheelDirection, binding.wheelThreshold};
    return result;
}

bool Input::RestoreWheelBindings(const WheelBindingMap& bindings) {
    if (bindings.size() > 1'024) return false;
    for (const auto& [action, binding] : bindings) {
        if (action.empty() || action.size() > 512 ||
            action.find('\0') != std::string::npos ||
            !std::isfinite(binding.threshold) || binding.threshold < 0.0f ||
            binding.threshold > 100.0f ||
            (binding.direction != WheelDirection::None &&
             binding.direction != WheelDirection::Up &&
             binding.direction != WheelDirection::Down) ||
            (binding.direction == WheelDirection::None &&
             binding.threshold != 0.0f)) return false;
    }
    auto named = m_namedBindings;
    WheelBindingMap pending;
    for (const auto& [action, binding] : bindings) {
        if (const auto found = named.find(action); found != named.end()) {
            found->second.wheelDirection = binding.direction;
            found->second.wheelThreshold = binding.threshold;
        } else {
            pending.emplace(action, binding);
        }
    }
    m_namedBindings = std::move(named);
    m_pendingWheelBindings = std::move(pending);
    return true;
}

bool Input::SetDefaultWheelBinding(const std::string_view action,
                                   const WheelDirection direction,
                                   const float threshold) {
    if (!HasAction(action) || !std::isfinite(threshold) ||
        threshold < 0.0f || threshold > 100.0f ||
        (direction != WheelDirection::None && direction != WheelDirection::Up &&
         direction != WheelDirection::Down) ||
        (direction == WheelDirection::None && threshold != 0.0f)) return false;
    m_defaultWheelBindings[std::string(action)] = {direction, threshold};
    return true;
}

void Input::ResetWheelBindings() {
    const Input defaults;
    for (auto& [action, bindings] : m_namedBindings) {
        WheelBinding fallback;
        if (const auto authored = m_defaultWheelBindings.find(action);
            authored != m_defaultWheelBindings.end()) {
            fallback = authored->second;
        } else if (const auto builtin = defaults.m_namedBindings.find(action);
                   builtin != defaults.m_namedBindings.end()) {
            fallback = {builtin->second.wheelDirection,
                        builtin->second.wheelThreshold};
        }
        bindings.wheelDirection = fallback.direction;
        bindings.wheelThreshold = fallback.threshold;
    }
    m_pendingWheelBindings.clear();
}

bool Input::SetActionContext(const std::string_view action,
                             std::string context) {
    const auto found = m_namedBindings.find(action);
    if (found == m_namedBindings.end() || context.empty() ||
        context.size() > 128 || context.find('\0') != std::string::npos)
        return false;
    found->second.context = std::move(context);
    return true;
}

bool Input::SetBaseContext(std::string context) {
    if (context.empty() || context.size() > 128 ||
        context.find('\0') != std::string::npos)
        return false;
    m_baseContext = std::move(context);
    return true;
}

bool Input::PushContext(std::string context) {
    if (context.empty() || context.size() > 128 ||
        context.find('\0') != std::string::npos || m_contextStack.size() >= 32)
        return false;
    m_contextStack.push_back(std::move(context));
    return true;
}

bool Input::PopContext() {
    if (m_contextStack.empty()) return false;
    m_contextStack.pop_back();
    return true;
}

std::vector<std::string> Input::ActiveContexts() const {
    std::vector<std::string> result{"global"};
    if (m_baseContext != "global") result.push_back(m_baseContext);
    result.insert(result.end(), m_contextStack.begin(), m_contextStack.end());
    return result;
}

bool Input::ActionActive(const std::string_view action) const {
    const auto found = m_namedBindings.find(action);
    if (found == m_namedBindings.end()) return false;
    const auto& context = found->second.context;
    return context == "global" || context == m_baseContext ||
           std::ranges::find(m_contextStack, context) != m_contextStack.end();
}

std::optional<int> Input::KeyCode(const std::string_view name) {
    if (name.empty() || name.find('\0') != std::string_view::npos)
        return std::nullopt;
    // The legacy GameCatalog used "Enter" for SDL's "Return" scancode.
    const auto normalized = name == "Enter" ? std::string_view{"Return"} : name;
    const auto scancode = SDL_GetScancodeFromName(std::string(normalized).c_str());
    if (scancode == SDL_SCANCODE_UNKNOWN) return std::nullopt;
    return static_cast<int>(scancode);
}

std::string Input::KeyName(const int scancode) {
    if (scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT)
        return {};
    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
    return name ? std::string(name) : std::string{};
}

std::vector<int> Input::KeyBindings(const std::string_view action) const {
    const auto found = m_namedBindings.find(action);
    if (found == m_namedBindings.end()) return {};
    return found->second.keyOrder;
}

bool Input::SetDefaultKeyboardBindings(const std::string_view action,
                                       const std::vector<int>& keys) {
    if (!HasAction(action) || keys.size() > 32) return false;
    std::set<int> unique;
    for (const int scancode : keys)
        if (scancode <= SDL_SCANCODE_UNKNOWN ||
            scancode >= SDL_SCANCODE_COUNT || !unique.insert(scancode).second)
            return false;
    m_defaultKeyboardBindings[std::string(action)] = keys;
    return true;
}

KeyRebindResult Input::RebindKey(const std::string_view action,
                                 const int scancode,
                                 const bool allowConflict) {
    const auto found = m_namedBindings.find(action);
    if (found == m_namedBindings.end())
        return {.status = KeyRebindStatus::UnknownAction};
    if (scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT)
        return {.status = KeyRebindStatus::InvalidScancode};

    KeyRebindResult result{.status = KeyRebindStatus::Applied};
    for (const auto& [otherAction, bindings] : m_namedBindings) {
        if (otherAction != action && bindings.keys.contains(scancode))
            result.conflicts.push_back(otherAction);
    }
    if (!allowConflict && !result.conflicts.empty() &&
        !found->second.keys.contains(scancode)) {
        result.status = KeyRebindStatus::Conflict;
        return result;
    }
    found->second.keys = {scancode};
    found->second.keyOrder = {scancode};
    if (const auto builtin = BuiltinAction(action))
        m_keyBindings[ActionIndex(*builtin)] = {scancode};
    return result;
}

KeyRebindResult Input::AddKeyBinding(const std::string_view action,
                                     const int scancode,
                                     const bool allowConflict) {
    const auto found = m_namedBindings.find(action);
    if (found == m_namedBindings.end())
        return {.status = KeyRebindStatus::UnknownAction};
    if (scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT)
        return {.status = KeyRebindStatus::InvalidScancode};

    KeyRebindResult result{.status = KeyRebindStatus::Applied};
    for (const auto& [otherAction, bindings] : m_namedBindings) {
        if (otherAction != action && bindings.keys.contains(scancode))
            result.conflicts.push_back(otherAction);
    }
    if (found->second.keys.contains(scancode)) return result;
    if (!allowConflict && !result.conflicts.empty()) {
        result.status = KeyRebindStatus::Conflict;
        return result;
    }
    if (found->second.keyOrder.size() >= 32) {
        result.status = KeyRebindStatus::LimitReached;
        return result;
    }
    found->second.keys.insert(scancode);
    found->second.keyOrder.push_back(scancode);
    if (const auto builtin = BuiltinAction(action))
        m_keyBindings[ActionIndex(*builtin)].insert(scancode);
    return result;
}

KeyRebindResult Input::ReplaceKeyBinding(const std::string_view action,
                                         const int oldScancode,
                                         const int newScancode,
                                         const bool allowConflict) {
    const auto found = m_namedBindings.find(action);
    if (found == m_namedBindings.end())
        return {.status = KeyRebindStatus::UnknownAction};
    if (oldScancode <= SDL_SCANCODE_UNKNOWN ||
        oldScancode >= SDL_SCANCODE_COUNT ||
        newScancode <= SDL_SCANCODE_UNKNOWN ||
        newScancode >= SDL_SCANCODE_COUNT)
        return {.status = KeyRebindStatus::InvalidScancode};
    auto position = std::ranges::find(found->second.keyOrder, oldScancode);
    if (position == found->second.keyOrder.end())
        return {.status = KeyRebindStatus::MissingBinding};
    if (oldScancode == newScancode)
        return {.status = KeyRebindStatus::Applied};
    if (found->second.keys.contains(newScancode))
        return {.status = KeyRebindStatus::Conflict};
    KeyRebindResult result{.status = KeyRebindStatus::Applied};
    for (const auto& [otherAction, bindings] : m_namedBindings)
        if (otherAction != action && bindings.keys.contains(newScancode))
            result.conflicts.push_back(otherAction);
    if (!allowConflict && !result.conflicts.empty()) {
        result.status = KeyRebindStatus::Conflict;
        return result;
    }
    found->second.keys.erase(oldScancode);
    found->second.keys.insert(newScancode);
    *position = newScancode;
    if (const auto builtin = BuiltinAction(action)) {
        auto& keys = m_keyBindings[ActionIndex(*builtin)];
        keys.erase(oldScancode);
        keys.insert(newScancode);
    }
    return result;
}

bool Input::RemoveKeyBinding(const std::string_view action,
                             const int scancode) {
    const auto found = m_namedBindings.find(action);
    if (found == m_namedBindings.end() ||
        scancode <= SDL_SCANCODE_UNKNOWN || scancode >= SDL_SCANCODE_COUNT ||
        found->second.keys.erase(scancode) == 0)
        return false;
    std::erase(found->second.keyOrder, scancode);
    if (const auto builtin = BuiltinAction(action))
        m_keyBindings[ActionIndex(*builtin)].erase(scancode);
    return true;
}

bool Input::BeginKeyCapture() {
    if (m_keyCaptureActive || m_capturedKey ||
        m_wheelCaptureActive || m_capturedWheel) return false;
    m_keyCaptureActive = true;
    return true;
}

std::optional<int> Input::TakeCapturedKey() {
    auto key = m_capturedKey;
    m_capturedKey.reset();
    return key;
}

void Input::CancelKeyCapture() {
    m_keyCaptureActive = false;
    m_capturedKey.reset();
}

bool Input::BeginWheelCapture() {
    if (m_wheelCaptureActive || m_capturedWheel ||
        m_keyCaptureActive || m_capturedKey) return false;
    m_wheelCaptureActive = true;
    return true;
}

std::optional<WheelDirection> Input::TakeCapturedWheel() {
    auto direction = m_capturedWheel;
    m_capturedWheel.reset();
    return direction;
}

void Input::CancelWheelCapture() {
    m_wheelCaptureActive = false;
    m_capturedWheel.reset();
}

KeyboardBindingMap Input::CaptureKeyboardBindings() const {
    KeyboardBindingMap result = m_pendingKeyBindings;
    for (const auto& [action, bindings] : m_namedBindings)
        result[action] = bindings.keyOrder;
    return result;
}

bool Input::RestoreKeyboardBindings(const KeyboardBindingMap& bindings) {
    if (bindings.size() > 1'024) return false;
    for (const auto& [action, keys] : bindings) {
        if (action.empty() || action.size() > 512 ||
            action.find('\0') != std::string::npos || keys.size() > 32)
            return false;
        std::set<int> unique;
        for (const int scancode : keys) {
            if (scancode <= SDL_SCANCODE_UNKNOWN ||
                scancode >= SDL_SCANCODE_COUNT || !unique.insert(scancode).second)
                return false;
        }
    }
    auto named = m_namedBindings;
    auto builtins = m_keyBindings;
    KeyboardBindingMap pending;
    for (const auto& [action, keys] : bindings) {
        const auto found = named.find(action);
        if (found == named.end()) {
            pending.emplace(action, keys);
            continue;
        }
        found->second.keys = {keys.begin(), keys.end()};
        found->second.keyOrder = keys;
        if (const auto builtin = BuiltinAction(action))
            builtins[ActionIndex(*builtin)] = found->second.keys;
    }
    m_namedBindings = std::move(named);
    m_keyBindings = std::move(builtins);
    m_pendingKeyBindings = std::move(pending);
    return true;
}

bool Input::RestoreUserBindings(const KeyboardBindingMap& keyboard,
                                const WheelBindingMap& wheel) {
    Input candidate = *this;
    candidate.ResetKeyboardBindings();
    candidate.ResetWheelBindings();
    if (!candidate.RestoreKeyboardBindings(keyboard) ||
        !candidate.RestoreWheelBindings(wheel)) return false;
    m_namedBindings.swap(candidate.m_namedBindings);
    m_keyBindings.swap(candidate.m_keyBindings);
    m_pendingKeyBindings.swap(candidate.m_pendingKeyBindings);
    m_pendingWheelBindings.swap(candidate.m_pendingWheelBindings);
    return true;
}

void Input::ResetKeyboardBindings() {
    const Input defaults;
    for (auto& [action, bindings] : m_namedBindings) {
        if (const auto authored = m_defaultKeyboardBindings.find(action);
            authored != m_defaultKeyboardBindings.end()) {
            bindings.keys = {authored->second.begin(), authored->second.end()};
            bindings.keyOrder = authored->second;
        } else {
            const auto found = defaults.m_namedBindings.find(action);
            bindings.keys = found == defaults.m_namedBindings.end()
                                ? std::set<int>{} : found->second.keys;
            bindings.keyOrder = found == defaults.m_namedBindings.end()
                                    ? std::vector<int>{} : found->second.keyOrder;
        }
    }
    m_keyBindings = defaults.m_keyBindings;
    for (std::size_t index = 0; index < kActionCount; ++index) {
        const auto action = static_cast<InputAction>(index);
        if (const auto found = m_namedBindings.find(ActionName(action));
            found != m_namedBindings.end())
            m_keyBindings[index] = found->second.keys;
    }
    m_pendingKeyBindings.clear();
}

bool Input::ActionPressed(const std::string_view action) const {
    if (!ActionActive(action)) return false;
    bool emitted = false;
    for (const InputEvent& event : m_events) {
        if (event.type != InputEventType::Action || event.action != action)
            continue;
        emitted = true;
        if (!EventDefaultSuppressed(event)) return true;
    }
    if (emitted) return false;
    const auto found = m_namedBindings.find(action);
    if (found == m_namedBindings.end()) return false;
    return std::ranges::any_of(found->second.keys, [this](const int key) {
               return m_keysPressed.contains(key) &&
                      !m_suppressedKeys.contains(key);
           }) ||
           std::ranges::any_of(found->second.gamepadButtons,
                              [this](const int button) {
                                  return m_gamepadButtonsPressed.contains(button);
                              });
}

bool Input::ActionDown(const std::string_view action) const {
    if (!ActionActive(action)) return false;
    // Injected actions behave as a one-frame down state. Physical sources use
    // the held sets below so down+up in one frame does not leave a false hold.
    for (const InputEvent& event : m_events) {
        if (event.type != InputEventType::Action || event.action != action)
            continue;
        if (!m_eventSources.contains(event.id) &&
            !EventDefaultSuppressed(event))
            return true;
    }
    if ((action == "accept" || action == "advance") && LeftDown()) return true;
    if ((action == "cancel" || action == "menu") && RightDown()) return true;
    const auto found = m_namedBindings.find(action);
    if (found == m_namedBindings.end()) return false;
    return std::ranges::any_of(found->second.keys, [this](const int key) {
               return m_keysDown.contains(key) &&
                      !m_suppressedKeys.contains(key);
           }) ||
           std::ranges::any_of(found->second.gamepadButtons,
                              [this](const int button) {
                                  return m_gamepadButtonsDown.contains(button) &&
                                         !m_suppressedGamepadButtons.contains(
                                             button);
                              });
}

bool Input::ActionPressed(const InputAction action) const {
    return action != InputAction::Count && ActionPressed(ActionName(action));
}

bool Input::ActionDown(const InputAction action) const {
    return action != InputAction::Count && ActionDown(ActionName(action));
}

bool Input::ActionPressedWithoutKeyboard(const InputAction action) const {
    if(action==InputAction::Count)return false;
    const std::string_view name = ActionName(action);
    if (!ActionActive(name)) return false;
    for (const InputEvent& event : m_events) {
        if (event.type != InputEventType::Action || event.action != name)
            continue;
        const auto source = m_eventSources.find(event.id);
        if (source != m_eventSources.end() &&
            source->second.kind == EventSourceKind::Key)
            continue;
        if (!EventDefaultSuppressed(event)) return true;
    }
    return false;
}

bool Input::ActionPressedWithoutPointer(const InputAction action) const {
    if (action == InputAction::Count) return false;
    return ActionPressedWithoutPointer(ActionName(action));
}

bool Input::ActionPressedWithoutPointer(const std::string_view name) const {
    if (!ActionActive(name)) return false;
    bool emitted = false;
    for (const InputEvent& event : m_events) {
        if (event.type != InputEventType::Action || event.action != name)
            continue;
        const auto source = m_eventSources.find(event.id);
        if (source != m_eventSources.end() &&
            source->second.kind == EventSourceKind::Pointer)
            continue;
        emitted = true;
        if (!EventDefaultSuppressed(event)) return true;
    }
    if (emitted) return false;
    const auto found = m_namedBindings.find(name);
    if (found == m_namedBindings.end()) return false;
    return std::ranges::any_of(found->second.keys, [this](const int key) {
               return m_keysPressed.contains(key) && !m_suppressedKeys.contains(key);
           }) ||
           std::ranges::any_of(found->second.gamepadButtons, [this](const int button) {
               return m_gamepadButtonsPressed.contains(button);
           });
}

void Input::InjectAction(const InputAction action) {
    if(action==InputAction::Count)return;
    m_injectedActions.insert(action);
    (void)InjectAction(ActionName(action));
}

bool Input::InjectAction(const std::string_view action) {
    if (!HasAction(action)) return false;
    const std::string name(action);
    m_injectedNamedActions.insert(name);
    if (const auto builtin = BuiltinAction(action))
        m_injectedActions.insert(*builtin);
    InputEvent event;
    event.type = InputEventType::Action;
    event.action = name;
    (void)AppendEvent(std::move(event));
    return true;
}

void Input::InjectKeyPress(const int scancode,const bool held) {
    if(scancode<0)return;
    if (m_keyCaptureActive && scancode > SDL_SCANCODE_UNKNOWN &&
        scancode < SDL_SCANCODE_COUNT) {
        m_capturedKey = scancode;
        m_keyCaptureActive = false;
        m_suppressText = true;
        return;
    }
    m_keysPressed.insert(scancode);
    if(held)m_keysDown.insert(scancode);
    AppendActionEventsForKey(scancode);
}

void Input::InjectText(const std::string_view text) {
    if (text.empty()) return;
    m_textInput += text;
    InputEvent inputEvent;
    inputEvent.type = InputEventType::TextInput;
    inputEvent.text = std::string(text);
    (void)AppendEvent(std::move(inputEvent));
}

void Input::InjectTap(const float x, const float y) {
    constexpr std::uint64_t kInjectedPointer = 0x5141;  // "QA"
    AppendPrimaryPress(x, y, kInjectedPointer);
    AppendPrimaryRelease(x, y, kInjectedPointer);
}

void Input::InjectSecondaryClick(const float x, const float y) {
    AppendSecondaryClick(x, y);
}

void Input::InjectFrame(const float mouseX, const float mouseY,
                        const bool leftClick) {
    const bool wasLeftDown = m_leftDown;
    NewFrame();
    m_keysDown.clear();
    m_mouseX = mouseX;
    m_mouseY = mouseY;
    m_leftClick = leftClick;
    m_leftReleased = wasLeftDown && !leftClick;
    m_leftDown = leftClick;
    m_rightDown = false;

    InputEvent move;
    move.type = InputEventType::PointerMove;
    move.x = mouseX;
    move.y = mouseY;
    (void)AppendEvent(std::move(move));
    if (leftClick) {
        InputEvent down;
        down.type = InputEventType::PointerDown;
        down.button = PointerButton::Primary;
        down.x = mouseX;
        down.y = mouseY;
        const std::uint64_t sourceId = AppendEvent(
            std::move(down),
            {.kind=EventSourceKind::Pointer,
             .pointerButton=PointerButton::Primary});
        AppendPointerDefaultActions(PointerButton::Primary, sourceId);
    } else if (m_leftReleased) {
        InputEvent up;
        up.type = InputEventType::PointerUp;
        up.button = PointerButton::Primary;
        up.x = mouseX;
        up.y = mouseY;
        (void)AppendEvent(std::move(up),
                          {.kind=EventSourceKind::Pointer,
                           .pointerButton=PointerButton::Primary});
    }
}

void Input::NewFrame() {
    ++m_frameSequence;
    m_leftClick = false;
    m_leftReleased = false;
    m_rightClick = false;
    m_wheelY = 0.0f;
    m_wheelUp = 0.0f;
    m_wheelDown = 0.0f;
    m_wheelTriggeredActions.clear();
    m_keysPressed.clear();
    m_gamepadButtonsPressed.clear();
    m_injectedActions.clear();
    m_injectedNamedActions.clear();
    m_textInput.clear();
    m_events.clear();
    m_consumedEvents.clear();
    m_suppressedDefaults.clear();
    m_eventSources.clear();
    m_suppressedKeys.clear();
    m_suppressedGamepadButtons.clear();
    m_suppressLeftPointer = false;
    m_suppressRightPointer = false;
    m_suppressWheel = false;
    m_suppressText = false;
}

bool Input::HasCurrentEvent(const std::uint64_t eventId) const {
    return std::ranges::any_of(m_events, [eventId](const InputEvent& event) {
        return event.id == eventId;
    });
}

std::uint64_t Input::AppendEvent(InputEvent event) {
    return AppendEvent(std::move(event), EventSource{});
}

std::uint64_t Input::AppendEvent(InputEvent event, const EventSource source) {
    event.id = m_nextEventId++;
    event.frame = m_frameSequence;
    const std::uint64_t id = event.id;
    m_events.push_back(std::move(event));
    if (source.kind != EventSourceKind::None) m_eventSources[id] = source;
    return id;
}

bool Input::ConsumeEvent(const std::uint64_t eventId) {
    return HasCurrentEvent(eventId) && m_consumedEvents.insert(eventId).second;
}

bool Input::SuppressDefault(const std::uint64_t eventId) {
    const auto found = std::ranges::find_if(
        m_events, [eventId](const InputEvent& event) {
            return event.id == eventId;
        });
    if (found == m_events.end() ||
        !m_suppressedDefaults.insert(eventId).second)
        return false;

    const std::uint64_t sourceId = found->sourceId != 0
                                       ? found->sourceId
                                       : (found->type == InputEventType::PointerDown ||
                                                  found->type == InputEventType::PointerUp ||
                                                  found->type == InputEventType::PointerWheel
                                              ? found->id
                                              : 0);
    if (sourceId != 0) {
        for (const InputEvent& event : m_events)
            if (event.id == sourceId || event.sourceId == sourceId)
                m_suppressedDefaults.insert(event.id);
    }

    const auto source = m_eventSources.find(found->id);
    if (source != m_eventSources.end()) {
        if (source->second.kind == EventSourceKind::Key) {
            m_suppressedKeys.insert(source->second.code);
            for (const auto& [id, other] : m_eventSources)
                if (other.kind == EventSourceKind::Key &&
                    other.code == source->second.code)
                    m_suppressedDefaults.insert(id);
        } else if (source->second.kind == EventSourceKind::GamepadButton) {
            m_suppressedGamepadButtons.insert(source->second.code);
            for (const auto& [id, other] : m_eventSources)
                if (other.kind == EventSourceKind::GamepadButton &&
                    other.code == source->second.code)
                    m_suppressedDefaults.insert(id);
        } else if (source->second.kind == EventSourceKind::Pointer) {
            if (source->second.pointerButton == PointerButton::Primary)
                m_suppressLeftPointer = true;
            else if (source->second.pointerButton == PointerButton::Secondary)
                m_suppressRightPointer = true;
        } else if (source->second.kind == EventSourceKind::Wheel) {
            m_suppressWheel = true;
        }
    }
    if (found->type == InputEventType::PointerWheel) m_suppressWheel = true;
    if (m_suppressWheel) {
        for (const InputEvent& event : m_events) {
            const auto other = m_eventSources.find(event.id);
            if (event.type == InputEventType::PointerWheel ||
                (other != m_eventSources.end() &&
                 other->second.kind == EventSourceKind::Wheel))
                m_suppressedDefaults.insert(event.id);
        }
    }
    if (found->type == InputEventType::TextInput) m_suppressText = true;
    return true;
}

bool Input::EventConsumed(const std::uint64_t eventId) const {
    return m_consumedEvents.contains(eventId);
}

bool Input::DefaultSuppressed(const std::uint64_t eventId) const {
    return m_suppressedDefaults.contains(eventId);
}

bool Input::EventDefaultSuppressed(const InputEvent& event) const {
    return DefaultSuppressed(event.id);
}

void Input::AppendActionEventsForKey(const int scancode) {
    for (const auto& [action, bindings] : m_namedBindings) {
        if (!bindings.keys.contains(scancode)) continue;
        InputEvent event;
        event.type = InputEventType::Action;
        event.action = action;
        (void)AppendEvent(std::move(event),
                          {.kind=EventSourceKind::Key,.code=scancode});
    }
}

void Input::AppendActionEventsForGamepadButton(const int button) {
    for (const auto& [action, bindings] : m_namedBindings) {
        if (!bindings.gamepadButtons.contains(button)) continue;
        InputEvent event;
        event.type = InputEventType::Action;
        event.action = action;
        (void)AppendEvent(
            std::move(event),
            {.kind=EventSourceKind::GamepadButton,.code=button});
    }
}

void Input::AppendPointerDefaultActions(const PointerButton button,
                                        const std::uint64_t sourceId) {
    const std::array<std::string_view,2> actions =
        button == PointerButton::Primary
            ? std::array<std::string_view,2>{"accept","advance"}
            : std::array<std::string_view,2>{"cancel","menu"};
    for (const std::string_view action : actions) {
        if (!HasAction(action)) continue;
        InputEvent event;
        event.type = InputEventType::Action;
        event.action = action;
        event.sourceId = sourceId;
        (void)AppendEvent(std::move(event),
                          {.kind=EventSourceKind::Pointer,
                           .pointerButton=button});
    }
}

void Input::AppendWheelActions(const std::uint64_t sourceId) {
    if (m_suppressWheel) return;
    for (const auto& [action, bindings] : m_namedBindings) {
        if (bindings.wheelDirection == WheelDirection::None ||
            m_wheelTriggeredActions.contains(action)) continue;
        const float travel = bindings.wheelDirection == WheelDirection::Up
                                 ? m_wheelUp : m_wheelDown;
        if (travel <= 0.0f || travel < bindings.wheelThreshold) continue;
        InputEvent event;
        event.type = InputEventType::Action;
        event.action = action;
        event.sourceId = sourceId;
        (void)AppendEvent(std::move(event), {.kind=EventSourceKind::Wheel});
        m_wheelTriggeredActions.insert(action);
    }
}

void Input::SetTouchProfile(const TouchProfile profile) {
    if (profile == m_touchProfile) return;
    m_touchProfile = profile;
    m_gestures = TouchGestureRecognizer{};
    m_primaryFinger.reset();
}

bool Input::TouchActive() const {
    switch (m_touchProfile) {
    case TouchProfile::On: return true;
    case TouchProfile::Off: return false;
    case TouchProfile::Auto: return m_lastPointerWasTouch;
    }
    return false;
}

std::optional<bool> Input::TakeTouchActiveChange() {
    const bool active = TouchActive();
    if (m_reportedTouch == active) return std::nullopt;
    m_reportedTouch = active;
    return active;
}

std::pair<float, float> Input::FingerPosition(const float normalizedX,
                                              const float normalizedY,
                                              const std::uint32_t windowId) const {
    float x = 0.0f, y = 0.0f;
    if (m_touchMapper && m_touchMapper(normalizedX, normalizedY, x, y))
        return {x, y};
    int width = 1, height = 1;
    if (auto* window = SDL_GetWindowFromID(windowId))
        (void)SDL_GetWindowSize(window, &width, &height);
    return {normalizedX * static_cast<float>(width),
            normalizedY * static_cast<float>(height)};
}

void Input::AppendPrimaryPress(const float x, const float y,
                               const std::uint64_t pointerId) {
    m_mouseX = x;
    m_mouseY = y;
    m_leftDown = true;
    m_leftClick = true;
    InputEvent inputEvent;
    inputEvent.type = InputEventType::PointerDown;
    inputEvent.pointerId = pointerId;
    inputEvent.button = PointerButton::Primary;
    inputEvent.x = x;
    inputEvent.y = y;
    const std::uint64_t sourceId = AppendEvent(
        std::move(inputEvent),
        {.kind=EventSourceKind::Pointer, .pointerButton=PointerButton::Primary});
    AppendPointerDefaultActions(PointerButton::Primary, sourceId);
}

void Input::AppendPrimaryRelease(const float x, const float y,
                                 const std::uint64_t pointerId) {
    m_mouseX = x;
    m_mouseY = y;
    m_leftReleased = m_leftDown;
    m_leftDown = false;
    InputEvent inputEvent;
    inputEvent.type = InputEventType::PointerUp;
    inputEvent.pointerId = pointerId;
    inputEvent.button = PointerButton::Primary;
    inputEvent.x = x;
    inputEvent.y = y;
    (void)AppendEvent(std::move(inputEvent),
                      {.kind=EventSourceKind::Pointer,
                       .pointerButton=PointerButton::Primary});
}

void Input::AppendSecondaryClick(const float x, const float y) {
    m_mouseX = x;
    m_mouseY = y;
    m_rightClick = true;
    InputEvent press;
    press.type = InputEventType::PointerDown;
    press.button = PointerButton::Secondary;
    press.x = x;
    press.y = y;
    const std::uint64_t sourceId = AppendEvent(
        std::move(press),
        {.kind=EventSourceKind::Pointer, .pointerButton=PointerButton::Secondary});
    AppendPointerDefaultActions(PointerButton::Secondary, sourceId);
    InputEvent release;
    release.type = InputEventType::PointerUp;
    release.button = PointerButton::Secondary;
    release.x = x;
    release.y = y;
    (void)AppendEvent(std::move(release),
                      {.kind=EventSourceKind::Pointer,
                       .pointerButton=PointerButton::Secondary});
}

void Input::AppendGestureAction(const std::string_view action) {
    if (!HasAction(action)) return;
    InputEvent event;
    event.type = InputEventType::Action;
    event.action = action;
    (void)AppendEvent(std::move(event), {.kind=EventSourceKind::Gesture});
}

void Input::ApplyGestures() {
    for (const TouchGestureEvent& gesture : m_gestures.Take()) {
        switch (gesture.gesture) {
        case TouchGesture::Tap:
            // A tap is a complete click: press and release in one frame,
            // after the gesture is known not to be a swipe or long press.
            AppendPrimaryPress(gesture.x, gesture.y, 0);
            AppendPrimaryRelease(gesture.x, gesture.y, 0);
            break;
        case TouchGesture::DragStart:
            AppendPrimaryPress(gesture.x, gesture.y, 0);
            break;
        case TouchGesture::LongPress:
            AppendGestureAction(ActionName(InputAction::ToggleUi));
            break;
        case TouchGesture::TwoFingerTap:
            // A two-finger tap is the touch right click: it backs out of
            // menus and does whatever a right click does in the game.
            AppendSecondaryClick(gesture.x, gesture.y);
            break;
        case TouchGesture::SwipeUp:
            AppendGestureAction(ActionName(InputAction::Backlog));
            break;
        }
    }
}

void Input::UpdateTouch(const std::uint64_t nowNs) {
    if (m_touchProfile == TouchProfile::Off) return;
    m_gestures.Update(nowNs);
    ApplyGestures();
}

void Input::ProcessGestureFinger(const SDL_Event& event) {
    const auto finger = static_cast<std::uint64_t>(event.tfinger.fingerID);
    const auto [x, y] = FingerPosition(event.tfinger.x, event.tfinger.y,
                                       event.tfinger.windowID);
    const std::uint64_t ns = event.tfinger.timestamp;
    m_lastPointerWasTouch = true;
    switch (event.type) {
    case SDL_EVENT_FINGER_DOWN: {
        // Hover follows the finger so the control under it highlights.
        m_mouseX = x;
        m_mouseY = y;
        InputEvent move;
        move.type = InputEventType::PointerMove;
        move.pointerId = finger;
        move.x = x;
        move.y = y;
        (void)AppendEvent(std::move(move));
        m_gestures.Down(finger, x, y, ns);
        ApplyGestures();
        break;
    }
    case SDL_EVENT_FINGER_MOTION: {
        m_gestures.Move(finger, x, y, ns);
        ApplyGestures();
        if (m_gestures.DragFinger() != finger) break;
        const float previousX = m_mouseX, previousY = m_mouseY;
        m_mouseX = x;
        m_mouseY = y;
        InputEvent move;
        move.type = InputEventType::PointerMove;
        move.pointerId = finger;
        move.x = x;
        move.y = y;
        move.deltaX = x - previousX;
        move.deltaY = y - previousY;
        (void)AppendEvent(std::move(move));
        break;
    }
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
        const bool dragging = m_gestures.DragFinger() == finger;
        if (event.type == SDL_EVENT_FINGER_CANCELED) m_gestures.Cancel(finger);
        else m_gestures.Up(finger, x, y, ns);
        ApplyGestures();
        if (dragging) AppendPrimaryRelease(x, y, finger);
        break;
    }
    default:
        break;
    }
}

void Input::Process(const SDL_Event& event) {
    // SDL also reports touches as synthetic mouse events. Fingers are
    // handled below, so those copies would double every tap.
    if ((event.type == SDL_EVENT_MOUSE_MOTION &&
         event.motion.which == SDL_TOUCH_MOUSEID) ||
        ((event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
          event.type == SDL_EVENT_MOUSE_BUTTON_UP) &&
         event.button.which == SDL_TOUCH_MOUSEID) ||
        (event.type == SDL_EVENT_MOUSE_WHEEL &&
         event.wheel.which == SDL_TOUCH_MOUSEID))
        return;
    if (m_touchProfile != TouchProfile::Off &&
        (event.type == SDL_EVENT_FINGER_DOWN ||
         event.type == SDL_EVENT_FINGER_MOTION ||
         event.type == SDL_EVENT_FINGER_UP ||
         event.type == SDL_EVENT_FINGER_CANCELED)) {
        ProcessGestureFinger(event);
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION ||
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        m_lastPointerWasTouch = false;
    switch (event.type) {
        case SDL_EVENT_QUIT:
            m_quit = true;
            break;

        case SDL_EVENT_MOUSE_MOTION:
            {
            InputEvent inputEvent;
            inputEvent.type = InputEventType::PointerMove;
            inputEvent.pointerId = 0;
            inputEvent.x = event.motion.x;
            inputEvent.y = event.motion.y;
            inputEvent.deltaX = event.motion.xrel;
            inputEvent.deltaY = event.motion.yrel;
            (void)AppendEvent(std::move(inputEvent));
            m_mouseX = event.motion.x;
            m_mouseY = event.motion.y;
            break;
            }

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            {
            PointerButton button = PointerButton::None;
            if (event.button.button == SDL_BUTTON_LEFT) {
                m_leftDown = true;
                m_leftClick = true;
                button = PointerButton::Primary;
            } else if (event.button.button == SDL_BUTTON_RIGHT) {
                m_rightDown = true;
                m_rightClick = true;
                button = PointerButton::Secondary;
            } else if (event.button.button == SDL_BUTTON_MIDDLE) {
                button = PointerButton::Middle;
            }
            if (button != PointerButton::None) {
                m_mouseX = event.button.x;
                m_mouseY = event.button.y;
                InputEvent inputEvent;
                inputEvent.type = InputEventType::PointerDown;
                inputEvent.button = button;
                inputEvent.x = event.button.x;
                inputEvent.y = event.button.y;
                const std::uint64_t sourceId = AppendEvent(
                    std::move(inputEvent),
                    {.kind=EventSourceKind::Pointer,
                     .pointerButton=button});
                if (button != PointerButton::Middle)
                    AppendPointerDefaultActions(button, sourceId);
            }
            break;
            }

        case SDL_EVENT_MOUSE_BUTTON_UP:
            {
            PointerButton button = PointerButton::None;
            if (event.button.button == SDL_BUTTON_LEFT) {
                m_leftDown = false;
                m_leftReleased = true;
                button = PointerButton::Primary;
            } else if (event.button.button == SDL_BUTTON_RIGHT) {
                m_rightDown = false;
                button = PointerButton::Secondary;
            } else if (event.button.button == SDL_BUTTON_MIDDLE) {
                button = PointerButton::Middle;
            }
            if (button != PointerButton::None) {
                m_mouseX = event.button.x;
                m_mouseY = event.button.y;
                InputEvent inputEvent;
                inputEvent.type = InputEventType::PointerUp;
                inputEvent.button = button;
                inputEvent.x = event.button.x;
                inputEvent.y = event.button.y;
                (void)AppendEvent(std::move(inputEvent),
                                  {.kind=EventSourceKind::Pointer,
                                   .pointerButton=button});
            }
            break;
            }

        case SDL_EVENT_MOUSE_WHEEL:
            {
            if (m_wheelCaptureActive || m_capturedWheel) {
                if (m_wheelCaptureActive && event.wheel.y != 0.0f) {
                    m_capturedWheel = event.wheel.y > 0.0f
                        ? WheelDirection::Up : WheelDirection::Down;
                    m_wheelCaptureActive = false;
                }
                // Suppress the gesture until the settings control takes it.
                // The old action and underlying UI scroll must not fire.
                break;
            }
            InputEvent inputEvent;
            inputEvent.type = InputEventType::PointerWheel;
            inputEvent.x = m_mouseX;
            inputEvent.y = m_mouseY;
            inputEvent.deltaX = event.wheel.x;
            inputEvent.deltaY = event.wheel.y;
            const std::uint64_t sourceId = AppendEvent(std::move(inputEvent));
            m_wheelY += event.wheel.y;
            if (event.wheel.y > 0.0f) m_wheelUp += event.wheel.y;
            else if (event.wheel.y < 0.0f) m_wheelDown -= event.wheel.y;
            AppendWheelActions(sourceId);
            break;
            }

        case SDL_EVENT_FINGER_DOWN:
            if(!m_primaryFinger){
                m_primaryFinger=static_cast<std::uint64_t>(
                    event.tfinger.fingerID);
                std::tie(m_mouseX, m_mouseY) = FingerPosition(
                    event.tfinger.x, event.tfinger.y, event.tfinger.windowID);
                m_leftDown=true;
                m_leftClick=true;
                InputEvent inputEvent;
                inputEvent.type = InputEventType::PointerDown;
                inputEvent.pointerId = *m_primaryFinger;
                inputEvent.button = PointerButton::Primary;
                inputEvent.x = m_mouseX;
                inputEvent.y = m_mouseY;
                const std::uint64_t sourceId = AppendEvent(
                    std::move(inputEvent),
                    {.kind=EventSourceKind::Pointer,
                     .pointerButton=PointerButton::Primary});
                AppendPointerDefaultActions(PointerButton::Primary, sourceId);
            }
            break;

        case SDL_EVENT_FINGER_MOTION:
            if(m_primaryFinger==static_cast<std::uint64_t>(
                   event.tfinger.fingerID)){
                int width=1,height=1;
                if(auto* window=SDL_GetWindowFromID(event.tfinger.windowID))
                    (void)SDL_GetWindowSize(window,&width,&height);
                std::tie(m_mouseX, m_mouseY) = FingerPosition(
                    event.tfinger.x, event.tfinger.y, event.tfinger.windowID);
                InputEvent inputEvent;
                inputEvent.type = InputEventType::PointerMove;
                inputEvent.pointerId = *m_primaryFinger;
                inputEvent.x = m_mouseX;
                inputEvent.y = m_mouseY;
                inputEvent.deltaX = event.tfinger.dx * static_cast<float>(width);
                inputEvent.deltaY = event.tfinger.dy * static_cast<float>(height);
                (void)AppendEvent(std::move(inputEvent));
            }
            break;

        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
            if(m_primaryFinger==static_cast<std::uint64_t>(
                   event.tfinger.fingerID)){
                std::tie(m_mouseX, m_mouseY) = FingerPosition(
                    event.tfinger.x, event.tfinger.y, event.tfinger.windowID);
                m_leftReleased=m_leftDown;
                m_leftDown=false;
                InputEvent inputEvent;
                inputEvent.type = InputEventType::PointerUp;
                inputEvent.pointerId = *m_primaryFinger;
                inputEvent.button = PointerButton::Primary;
                inputEvent.x = m_mouseX;
                inputEvent.y = m_mouseY;
                (void)AppendEvent(std::move(inputEvent),
                                  {.kind=EventSourceKind::Pointer,
                                   .pointerButton=PointerButton::Primary});
                m_primaryFinger.reset();
            }
            break;

        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            const int button=static_cast<int>(event.gbutton.button);
            if(!m_gamepadButtonsDown.contains(button))
                m_gamepadButtonsPressed.insert(button);
            m_gamepadButtonsDown.insert(button);
            AppendActionEventsForGamepadButton(button);
            break;
        }

        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            m_gamepadButtonsDown.erase(static_cast<int>(event.gbutton.button));
            break;

        case SDL_EVENT_KEY_DOWN:
            if (!event.key.repeat) {
                const int scancode = static_cast<int>(event.key.scancode);
                if (m_keyCaptureActive && scancode > SDL_SCANCODE_UNKNOWN &&
                    scancode < SDL_SCANCODE_COUNT) {
                    m_capturedKey = scancode;
                    m_keyCaptureActive = false;
                    m_suppressText = true;
                    break;
                }
                m_keysPressed.insert(scancode);
                AppendActionEventsForKey(scancode);
                // Android's Back button backs out the way a right click does.
                if (event.key.scancode == SDL_SCANCODE_AC_BACK)
                    AppendSecondaryClick(m_mouseX, m_mouseY);
            }
            m_keysDown.insert(static_cast<int>(event.key.scancode));
            break;

        case SDL_EVENT_KEY_UP:
            m_keysDown.erase(static_cast<int>(event.key.scancode));
            break;

        case SDL_EVENT_TEXT_INPUT:
            if (m_suppressText) break;
            m_textInput += event.text.text;
            {
            InputEvent inputEvent;
            inputEvent.type = InputEventType::TextInput;
            inputEvent.text = event.text.text;
            (void)AppendEvent(std::move(inputEvent));
            }
            break;

        default:
            break;
    }
}

}
