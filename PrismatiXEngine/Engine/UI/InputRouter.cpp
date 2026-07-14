#include "Engine/UI/InputRouter.h"

#include "Engine/Platform/Input.h"

#include <SDL3/SDL_scancode.h>

#include <algorithm>

namespace px::ui {

Control* InputRouter::HitTest(Control& root, Vec2 point) const {
    if (!root.IsVisibleInTree() || !root.Enabled()) return nullptr;
    std::vector<Control*> children;
    children.reserve(root.Children().size());
    for (const auto& child : root.Children())
        if (auto* control = dynamic_cast<Control*>(child.get())) children.push_back(control);
    std::stable_sort(children.begin(), children.end(), [](const Control* left, const Control* right) {
        return left->ZOrder() < right->ZOrder();
    });
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        if (auto* hit = HitTest(**it, point)) return hit;
    }
    return root.GetMouseFilter() != MouseFilter::Ignore && root.HitTest(point) ? &root : nullptr;
}

void InputRouter::Dispatch(Control* target, UIEvent event) {
    for (Control* node = target; node; node = dynamic_cast<Control*>(node->Parent())) {
        node->HandleEvent(event);
        node->EmitInputSignal(event);
        if (event.stopPropagation || (event.handled && node->GetMouseFilter() == MouseFilter::Stop)) break;
    }
}

void InputRouter::SetFocus(Control* control) {
    if (control && (!control->Enabled() || !control->IsVisibleInTree() || control->GetFocusMode() == FocusMode::None)) return;
    if (m_focused == control) return;
    if (m_focused) Dispatch(m_focused, UIEvent{.type = UIEventType::FocusLost});
    m_focused = control;
    if (m_focused) Dispatch(m_focused, UIEvent{.type = UIEventType::FocusGained});
    RefreshStates();
}

void InputRouter::CollectFocusable(Control& root, std::vector<Control*>& output) const {
    if (root.Enabled() && root.IsVisibleInTree() && root.GetFocusMode() == FocusMode::All) output.push_back(&root);
    for (const auto& child : root.Children()) {
        if (auto* control = dynamic_cast<Control*>(child.get())) CollectFocusable(*control, output);
    }
}

void InputRouter::FocusNext(bool reverse) {
    std::vector<Control*> controls;
    CollectFocusable(m_root, controls);
    if (controls.empty()) return SetFocus(nullptr);
    auto it = std::find(controls.begin(), controls.end(), m_focused);
    std::size_t index = it == controls.end() ? 0 : static_cast<std::size_t>(it - controls.begin());
    if (it != controls.end()) index = reverse ? (index + controls.size() - 1) % controls.size() : (index + 1) % controls.size();
    SetFocus(controls[index]);
}

void InputRouter::RefreshStates() {
    std::vector<Control*> stack{&m_root};
    while (!stack.empty()) {
        Control* node = stack.back(); stack.pop_back();
        node->SetInteractionState(node == m_hovered, node == m_captured, node == m_focused);
        for (const auto& child : node->Children()) if (auto* c = dynamic_cast<Control*>(child.get())) stack.push_back(c);
    }
}

void InputRouter::Update(const Input& input) {
    m_lastFrameConsumed = false;
    const Vec2 pointer{input.MouseX(), input.MouseY()};
    Control* hit = HitTest(m_root, pointer);
    if (hit != m_hovered) {
        if (m_hovered) Dispatch(m_hovered, UIEvent{.type = UIEventType::PointerExit, .position = pointer});
        m_hovered = hit;
        if (m_hovered) Dispatch(m_hovered, UIEvent{.type = UIEventType::PointerEnter, .position = pointer});
    }
    if (pointer != m_lastPointer) {
        Dispatch(m_captured ? m_captured : hit,
                 UIEvent{.type = UIEventType::PointerMove, .position = pointer,
                         .delta = {pointer.x - m_lastPointer.x, pointer.y - m_lastPointer.y}});
        m_lastPointer = pointer;
    }
    if (input.LeftClick()) {
        m_captured = hit;
        m_lastFrameConsumed = hit && hit->GetMouseFilter() == MouseFilter::Stop;
        if (hit && hit->GetFocusMode() != FocusMode::None) SetFocus(hit);
        Dispatch(m_captured, UIEvent{.type = UIEventType::PointerDown, .position = pointer});
    }
    if (input.LeftReleased()) {
        Control* captured = m_captured;
        Dispatch(captured, UIEvent{.type = UIEventType::PointerUp, .position = pointer});
        m_captured = nullptr;
        if (captured && captured == hit) { Dispatch(captured, UIEvent{.type = UIEventType::Click, .position = pointer}); m_lastFrameConsumed = true; }
    }
    if (input.WheelY() != 0.0f) Dispatch(hit, UIEvent{.type = UIEventType::Scroll, .position = pointer, .wheel = input.WheelY()});

    if (input.KeyPressed(SDL_SCANCODE_TAB)) FocusNext(input.KeyDown(SDL_SCANCODE_LSHIFT) || input.KeyDown(SDL_SCANCODE_RSHIFT));
    if (m_focused && (input.KeyPressed(SDL_SCANCODE_RETURN) || input.KeyPressed(SDL_SCANCODE_SPACE)))
        Dispatch(m_focused, UIEvent{.type = UIEventType::Activate});
    if (m_focused && input.KeyPressed(SDL_SCANCODE_ESCAPE)) Dispatch(m_focused, UIEvent{.type = UIEventType::Cancel});
    if (m_focused) {
        for (int key : input.PressedKeys()) {
            if (key == SDL_SCANCODE_TAB || key == SDL_SCANCODE_RETURN || key == SDL_SCANCODE_SPACE || key == SDL_SCANCODE_ESCAPE) continue;
            UIEvent event{.type=UIEventType::KeyDown}; event.key=key; Dispatch(m_focused,std::move(event));
        }
        if (!input.TextInput().empty()) { UIEvent event{.type=UIEventType::TextInput}; event.text=input.TextInput(); Dispatch(m_focused,std::move(event)); }
    }
    RefreshStates();
}

}  // namespace px::ui
