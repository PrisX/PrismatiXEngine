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
    void FocusDirection(Vec2 direction);
    Status PushFocusScope(Control& scope, bool modalCapture = false);
    bool PopFocusScope();
    [[nodiscard]] Control* ActiveFocusScope() const;
    [[nodiscard]] bool ModalCaptureActive() const;
    [[nodiscard]] Control* Focused() const { return m_focused; }
    [[nodiscard]] Control* Hovered() const { return m_hovered; }
    [[nodiscard]] Control* Captured() const { return m_captured; }
    [[nodiscard]] bool LastFrameConsumed() const { return m_lastFrameConsumed; }
    // Uses the same ancestor clipping and inverse-transform rules as pointer
    // dispatch. Exposed for deterministic UI/accessibility conformance tests.
    [[nodiscard]] Control* TargetAt(Vec2 point) const;

private:
    Control* HitTest(Control& root, Vec2 point) const;
    void Dispatch(Control* target, UIEvent event);
    void CollectFocusable(Control& root, std::vector<Control*>& output) const;
    [[nodiscard]] bool IsInside(const Control* control,
                                const Control* ancestor) const;
    void RefreshStates();

    struct FocusScope {
        Control* root = nullptr;
        Control* previousFocus = nullptr;
        bool modalCapture = false;
    };

    Control& m_root;
    Control* m_hovered = nullptr;
    Control* m_captured = nullptr;
    Control* m_focused = nullptr;
    Vec2 m_lastPointer{-1000, -1000};
    bool m_lastFrameConsumed = false;
    std::vector<FocusScope> m_focusScopes;
};

}  // namespace px::ui
