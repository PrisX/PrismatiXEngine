#pragma once

#include "Engine/UI/Control.h"

#include <vector>

namespace px { class Input; }

namespace px::ui {

class InputRouter {
public:
    explicit InputRouter(Control& root) : m_root(root) {}

    void Update(const Input& input);
    void SetFocus(Control* control);
    void FocusNext(bool reverse = false);
    [[nodiscard]] Control* Focused() const { return m_focused; }
    [[nodiscard]] Control* Hovered() const { return m_hovered; }
    [[nodiscard]] Control* Captured() const { return m_captured; }
    [[nodiscard]] bool LastFrameConsumed() const { return m_lastFrameConsumed; }

private:
    Control* HitTest(Control& root, Vec2 point) const;
    void Dispatch(Control* target, UIEvent event);
    void CollectFocusable(Control& root, std::vector<Control*>& output) const;
    void RefreshStates();

    Control& m_root;
    Control* m_hovered = nullptr;
    Control* m_captured = nullptr;
    Control* m_focused = nullptr;
    Vec2 m_lastPointer{-1000, -1000};
    bool m_lastFrameConsumed = false;
};

}  // namespace px::ui
