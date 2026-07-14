#include "Engine/UI/Control.h"

#include "Engine/Graphics/Renderer2D.h"
#include "Engine/UI/Theme.h"

#include <algorithm>
#include <cmath>

namespace px::ui {

StyleStateSet Control::ActiveStyleStates() const {
    StyleStateSet states;
    states.Set(StyleState::Checked, m_visualChecked);
    states.Set(StyleState::Selected, m_visualSelected);
    states.Set(StyleState::Focused, m_focused);
    states.Set(StyleState::Hover, m_hovered);
    states.Set(StyleState::Pressed, m_pressed);
    states.Set(StyleState::Disabled, !m_enabled);
    return states;
}
namespace {
float ClampAxis(float value, float minimum, float maximum) {
    return std::max(minimum, std::min(value, maximum));
}
}

const char* ChildLayoutPolicyName(ChildLayoutPolicy policy) {
    switch (policy) {
        case ChildLayoutPolicy::Free: return "自由佈局";
        case ChildLayoutPolicy::LinearX: return "水平排序";
        case ChildLayoutPolicy::LinearY: return "垂直排序";
        case ChildLayoutPolicy::Grid: return "網格排序";
        case ChildLayoutPolicy::Flow: return "流動排序";
        case ChildLayoutPolicy::SingleSlot: return "單一插槽";
        case ChildLayoutPolicy::Pages: return "頁面排序";
        case ChildLayoutPolicy::RuntimeManaged: return "執行期管理";
    }
    return "未知";
}

Rect ControlLayoutMath::ResolveChildRect(Rect parent, Rect anchors, Rect offsets,
                                         Vec2 desired) {
    const float left = parent.x + parent.w * anchors.x + offsets.x;
    const float top = parent.y + parent.h * anchors.y + offsets.y;
    const float right = parent.x + parent.w * anchors.w + offsets.w;
    const float bottom = parent.y + parent.h * anchors.h + offsets.h;
    const float width = anchors.w != anchors.x ? right - left : std::max(desired.x, offsets.w);
    const float height = anchors.h != anchors.y ? bottom - top : std::max(desired.y, offsets.h);
    return {left, top, std::max(0.0f, width), std::max(0.0f, height)};
}

Rect ControlLayoutMath::OffsetsForRect(Rect parent, Rect anchors, Rect child) {
    Rect offsets;
    offsets.x = child.x - (parent.x + parent.w * anchors.x);
    offsets.y = child.y - (parent.y + parent.h * anchors.y);
    offsets.w = anchors.w != anchors.x
                    ? child.x + child.w - (parent.x + parent.w * anchors.w)
                    : child.w;
    offsets.h = anchors.h != anchors.y
                    ? child.y + child.h - (parent.y + parent.h * anchors.h)
                    : child.h;
    return offsets;
}
void Control::SetAnchors(Rect value) { m_anchors = value; InvalidateLayout(); }
void Control::SetOffsets(Rect value) { m_offsets = value; InvalidateLayout(); }
void Control::SetCustomMinimumSize(Vec2 value) { m_customMinimum = value; InvalidateLayout(); }
void Control::SetMaximumSize(Vec2 value) { m_maximum = value; InvalidateLayout(); }
void Control::SetSizeFlags(SizeFlag h, SizeFlag v) { m_horizontalFlags = h; m_verticalFlags = v; InvalidateLayout(); }
void Control::SetStretchRatio(float value) { m_stretchRatio = std::max(0.001f, value); InvalidateLayout(); }
void Control::SetVisibility(Visibility value) { m_visibility = value; InvalidateLayout(); }

bool Control::IsVisibleInTree() const {
    if (m_visibility != Visibility::Visible) return false;
    for (auto* p = Parent(); p; p = p->Parent()) {
        if (const auto* control = dynamic_cast<const Control*>(p);
            control && control->GetVisibility() != Visibility::Visible) return false;
    }
    return true;
}

Vec2 Control::Measure(Vec2 available) {
    if (m_visibility == Visibility::Collapsed) return m_desiredSize = {};
    const Vec2 measured = MeasureOverride(available);
    m_desiredSize = {
        ClampAxis(measured.x, m_customMinimum.x, m_maximum.x),
        ClampAxis(measured.y, m_customMinimum.y, m_maximum.y)
    };
    return m_desiredSize;
}

void Control::Arrange(Rect finalRect) {
    m_layoutRect = finalRect;
    m_layoutDirty = false;
    ArrangeOverride(finalRect);
}

void Control::InvalidateLayout() {
    if (m_layoutDirty) return;
    m_layoutDirty = true;
    if (auto* parent = dynamic_cast<Control*>(Parent())) parent->InvalidateLayout();
}

bool Control::HitTest(Vec2 point) const {
    if(!IsVisibleInTree()||!m_enabled||m_mouseFilter==MouseFilter::Ignore)return false;
    std::vector<const Control*> chain;for(const Control* control=this;control;control=dynamic_cast<const Control*>(control->Parent()))chain.push_back(control);std::reverse(chain.begin(),chain.end());
    for(const auto* control:chain){const float sx=std::abs(control->m_scale.x)<.0001f?.0001f:control->m_scale.x,sy=std::abs(control->m_scale.y)<.0001f?.0001f:control->m_scale.y;const Vec2 pivot{control->m_layoutRect.x+control->m_pivot.x*control->m_layoutRect.w,control->m_layoutRect.y+control->m_pivot.y*control->m_layoutRect.h};const float radians=-control->m_rotation*3.14159265358979323846f/180.0f,cs=std::cos(radians),sn=std::sin(radians);const float x=point.x-pivot.x,y=point.y-pivot.y;point={pivot.x+(cs*x-sn*y)/sx,pivot.y+(sn*x+cs*y)/sy};}
    return m_layoutRect.Contains(point.x,point.y);
}

void Control::HandleEvent(UIEvent&) {}

Control::SignalConnection Control::ConnectSignal(std::string signal, SignalHandler handler) {
    if (signal.empty() || !handler) return 0;
    const SignalConnection connection = m_nextSignalConnection++;
    m_signalHandlers[std::move(signal)].push_back({connection, std::move(handler)});
    return connection;
}

bool Control::DisconnectSignal(const std::string_view signal, const SignalConnection connection) {
    const auto found = m_signalHandlers.find(std::string(signal));
    if (found == m_signalHandlers.end()) return false;
    auto& handlers = found->second;
    const auto before = handlers.size();
    std::erase_if(handlers, [connection](const SignalSlot& slot) { return slot.id == connection; });
    const bool removed = handlers.size() != before;
    if (handlers.empty()) m_signalHandlers.erase(found);
    return removed;
}

void Control::EmitSignal(const std::string_view signal, const SignalArguments& arguments) {
    const auto found = m_signalHandlers.find(std::string(signal));
    if (found == m_signalHandlers.end()) return;
    // Handlers are copied so callbacks may safely connect/disconnect signals.
    const auto handlers = found->second;
    for (const auto& slot : handlers) if (slot.handler) slot.handler(arguments);
}

void Control::EmitInputSignal(const UIEvent& event) {
    SignalArguments arguments;
    switch (event.type) {
        case UIEventType::PointerEnter: EmitSignal("pointerEntered"); return;
        case UIEventType::PointerExit: EmitSignal("pointerExited"); return;
        case UIEventType::FocusGained: EmitSignal("focusEntered"); return;
        case UIEventType::FocusLost: EmitSignal("focusExited"); return;
        case UIEventType::PointerDown:
            arguments["position"] = event.position; EmitSignal("pointerDown", arguments); return;
        case UIEventType::PointerUp:
            arguments["position"] = event.position; EmitSignal("pointerUp", arguments); return;
        case UIEventType::Click:
            arguments["position"] = event.position; EmitSignal("clicked", arguments); return;
        case UIEventType::Scroll:
            arguments["value"] = static_cast<double>(event.wheel); EmitSignal("scrolled", arguments); return;
        default: return;
    }
}

void Control::Update(float deltaSeconds) {
    for (const auto& child : Children())
        if (auto* control = dynamic_cast<Control*>(child.get())) control->Update(deltaSeconds);
}

void Control::Draw(graphics::Renderer2D& renderer, const Theme& theme) {
    if (m_visibility != Visibility::Visible) return;
    Color tint=m_modulate;tint.a=static_cast<std::uint8_t>(static_cast<float>(tint.a)*m_opacity);
    renderer.PushTransform({m_layoutRect.x+m_pivot.x*m_layoutRect.w,m_layoutRect.y+m_pivot.y*m_layoutRect.h},m_scale,m_rotation,tint);
    if(m_clipContent)renderer.PushClip(m_layoutRect);
    DrawSelf(renderer, theme);
    std::vector<Control*> controls;for(const auto& child:Children())if(auto* control=dynamic_cast<Control*>(child.get()))controls.push_back(control);std::stable_sort(controls.begin(),controls.end(),[](const Control* left,const Control* right){return left->ZOrder()<right->ZOrder();});for(auto* control:controls)control->Draw(renderer,theme);
    if(m_clipContent)renderer.PopClip();renderer.PopTransform();
}

void Control::SetInteractionState(bool hovered, bool pressed, bool focused) {
    m_hovered = hovered;
    m_pressed = pressed;
    m_focused = focused;
}

Vec2 Control::MeasureOverride(Vec2 available) {
    Vec2 result{};
    for (const auto& child : Children()) {
        auto* control = dynamic_cast<Control*>(child.get());
        if (!control || control->GetVisibility() == Visibility::Collapsed) continue;
        const Vec2 desired = control->Measure(available);
        result.x = std::max(result.x, desired.x);
        result.y = std::max(result.y, desired.y);
    }
    return result;
}

void Control::ArrangeOverride(Rect finalRect) {
    for (const auto& child : Children()) {
        auto* control = dynamic_cast<Control*>(child.get());
        if (!control || control->GetVisibility() == Visibility::Collapsed) continue;
        control->Arrange(ControlLayoutMath::ResolveChildRect(
            finalRect, control->Anchors(), control->Offsets(), control->DesiredSize()));
    }
}

void Control::DrawSelf(graphics::Renderer2D&, const Theme&) {}

}  // namespace px::ui
