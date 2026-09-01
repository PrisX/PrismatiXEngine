#include "Engine/UI/InputRouter.h"

#include "Engine/Platform/Input.h"

#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace px::ui {

Control* InputRouter::HitTest(Control& root, Vec2 point) const {
    if (!root.IsVisibleInTree() || !root.Enabled()) return nullptr;
    // A clipped ancestor excludes its entire subtree. Control::HitTest applies
    // the full ancestor inverse-transform chain, so this remains correct for
    // rotated and scaled clip owners as well as axis-aligned controls.
    if (root.ClipContent() && !root.HitTest(point)) return nullptr;
    for (auto it = root.Children().rbegin(); it != root.Children().rend(); ++it) {
        if (auto* control = dynamic_cast<Control*>(it->get()))
            if (auto* hit = HitTest(*control, point)) return hit;
    }
    return root.GetMouseFilter() != MouseFilter::Ignore && root.HitTest(point) ? &root : nullptr;
}

Control* InputRouter::TargetAt(const Vec2 point) const {
    if(!m_focusScopes.empty()&&m_focusScopes.back().modalCapture){
        auto* scope=m_focusScopes.back().root;
        if(auto* hit=HitTest(*scope,point))return hit;
        // Modal capture consumes pointer input outside the modal instead of
        // allowing it to fall through to the obscured route.
        return scope;
    }
    return HitTest(m_root, point);
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
    if(control&&ActiveFocusScope()&&!IsInside(control,ActiveFocusScope()))return;
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
    CollectFocusable(ActiveFocusScope()?*ActiveFocusScope():m_root, controls);
    std::stable_sort(controls.begin(), controls.end(), [](const Control* left,
                                                         const Control* right) {
        return left->AccessibilityFocusOrder() < right->AccessibilityFocusOrder();
    });
    if (controls.empty()) return SetFocus(nullptr);
    auto it = std::find(controls.begin(), controls.end(), m_focused);
    std::size_t index = it == controls.end() ? 0 : static_cast<std::size_t>(it - controls.begin());
    if (it != controls.end()) index = reverse ? (index + controls.size() - 1) % controls.size() : (index + 1) % controls.size();
    SetFocus(controls[index]);
}

bool InputRouter::IsInside(const Control* control,const Control* ancestor) const {
    if(!control||!ancestor)return false;
    for(auto* current=control;current;
        current=dynamic_cast<const Control*>(current->Parent()))
        if(current==ancestor)return true;
    return false;
}

Status InputRouter::PushFocusScope(Control& scope,const bool modalCapture) {
    if(!IsInside(&scope,&m_root)||!scope.Enabled()||!scope.IsVisibleInTree())
        return Status::Fail(diag::Diagnostic{
            .severity=diag::Severity::Error,.code="PXUI2501",
            .category="UI.Input",
            .message="Focus scope must be a visible enabled descendant of the UI root"});
    if(const auto* active=ActiveFocusScope();active&&!IsInside(&scope,active))
        return Status::Fail(diag::Diagnostic{
            .severity=diag::Severity::Error,.code="PXUI2502",
            .category="UI.Input",
            .message="Nested focus scope must belong to the active focus scope"});
    m_focusScopes.push_back({&scope,m_focused,modalCapture});
    if(!m_focused||!IsInside(m_focused,&scope)){
        m_focused=nullptr;
        FocusNext();
    }
    RefreshStates();
    return Status::Ok();
}

bool InputRouter::PopFocusScope() {
    if(m_focusScopes.empty())return false;
    const auto previous=m_focusScopes.back().previousFocus;
    m_focusScopes.pop_back();
    m_focused=nullptr;
    if(previous&&previous->Enabled()&&previous->IsVisibleInTree()&&
       (!ActiveFocusScope()||IsInside(previous,ActiveFocusScope())))
        SetFocus(previous);
    else FocusNext();
    RefreshStates();
    return true;
}

Control* InputRouter::ActiveFocusScope() const {
    return m_focusScopes.empty()?nullptr:m_focusScopes.back().root;
}

bool InputRouter::ModalCaptureActive() const {
    return !m_focusScopes.empty()&&m_focusScopes.back().modalCapture;
}

void InputRouter::FocusDirection(const Vec2 direction) {
    if(direction==Vec2{})return;
    std::vector<Control*> controls;
    CollectFocusable(ActiveFocusScope()?*ActiveFocusScope():m_root,controls);
    if(controls.empty())return SetFocus(nullptr);
    if(!m_focused||!IsInside(m_focused,
         ActiveFocusScope()?ActiveFocusScope():&m_root)){
        SetFocus(controls.front());
        return;
    }
    const Rect current=m_focused->LayoutRect();
    const Vec2 origin{current.x+current.w*.5f,current.y+current.h*.5f};
    Control* best=nullptr;
    float bestScore=std::numeric_limits<float>::max();
    const float length=std::sqrt(direction.x*direction.x+
                                 direction.y*direction.y);
    const Vec2 unit{direction.x/length,direction.y/length};
    for(auto* candidate:controls){
        if(candidate==m_focused)continue;
        const Rect bounds=candidate->LayoutRect();
        const Vec2 delta{bounds.x+bounds.w*.5f-origin.x,
                         bounds.y+bounds.h*.5f-origin.y};
        const float forward=delta.x*unit.x+delta.y*unit.y;
        if(forward<=0.001f)continue;
        const float lateral=std::abs(delta.x*unit.y-delta.y*unit.x);
        const float score=forward+lateral*2.0f;
        if(score<bestScore){bestScore=score;best=candidate;}
    }
    if(best)SetFocus(best);
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
    Control* hit = TargetAt(pointer);
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

    const bool reverseTab=input.KeyPressed(SDL_SCANCODE_TAB)&&
        (input.KeyDown(SDL_SCANCODE_LSHIFT)||input.KeyDown(SDL_SCANCODE_RSHIFT));
    if(reverseTab||input.ActionPressed(InputAction::FocusPrevious)){
        FocusNext(true);m_lastFrameConsumed=true;
    }else if(input.ActionPressed(InputAction::FocusNext)){
        FocusNext(false);m_lastFrameConsumed=true;
    }
    const bool editing = m_focused && m_focused->CapturesTextInput();
    const bool shift = input.KeyDown(SDL_SCANCODE_LSHIFT) ||
                       input.KeyDown(SDL_SCANCODE_RSHIFT);
    const bool control = input.KeyDown(SDL_SCANCODE_LCTRL) ||
                         input.KeyDown(SDL_SCANCODE_RCTRL);
    const auto navigate = [&](const InputAction action, const int key,
                              const Vec2 direction) {
        const bool requested = editing
                                   ? input.KeyPressed(key) ||
                                         input.ActionPressedWithoutKeyboard(action)
                                   : input.ActionPressed(action);
        if (!requested) return;
        if (editing) {
            Dispatch(m_focused, UIEvent{.type = UIEventType::KeyDown,
                                        .key = key,
                                        .shift = shift,
                                        .control = control});
        } else {
            FocusDirection(direction);
        }
        m_lastFrameConsumed = true;
    };
    navigate(InputAction::NavigateUp, SDL_SCANCODE_UP, {0, -1});
    navigate(InputAction::NavigateDown, SDL_SCANCODE_DOWN, {0, 1});
    navigate(InputAction::NavigateLeft, SDL_SCANCODE_LEFT, {-1, 0});
    navigate(InputAction::NavigateRight, SDL_SCANCODE_RIGHT, {1, 0});
    if (m_focused && input.ActionPressed(InputAction::Accept) &&
        (!editing || !input.KeyPressed(SDL_SCANCODE_SPACE)))
        Dispatch(m_focused, UIEvent{.type = UIEventType::Activate,
                                    .shift = shift,
                                    .control = control});
    if (m_focused && input.ActionPressed(InputAction::Cancel))
        Dispatch(m_focused, UIEvent{.type = UIEventType::Cancel});
    if (m_focused) {
        for (int key : input.PressedKeys()) {
            if (key == SDL_SCANCODE_TAB || key == SDL_SCANCODE_RETURN ||
                key == SDL_SCANCODE_SPACE || key == SDL_SCANCODE_ESCAPE ||
                key == SDL_SCANCODE_UP || key == SDL_SCANCODE_DOWN ||
                key == SDL_SCANCODE_LEFT || key == SDL_SCANCODE_RIGHT) continue;
            UIEvent event{.type=UIEventType::KeyDown};
            event.key=key;
            event.shift=shift;
            event.control=control;
            Dispatch(m_focused,std::move(event));
        }
        if (!input.TextInput().empty()) {
            UIEvent event{.type=UIEventType::TextInput};
            event.text=input.TextInput();
            event.shift=shift;
            event.control=control;
            Dispatch(m_focused,std::move(event));
        }
    }
    RefreshStates();
}

}  // namespace px::ui
