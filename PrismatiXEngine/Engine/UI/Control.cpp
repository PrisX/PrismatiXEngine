#include "Engine/UI/Control.h"

#include "Engine/UI/Theme.h"

#include <algorithm>

namespace px::ui {
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
    return IsVisibleInTree() && m_enabled && m_mouseFilter != MouseFilter::Ignore &&
           m_layoutRect.Contains(point.x, point.y);
}

void Control::HandleEvent(UIEvent&) {}

void Control::Update(float deltaSeconds) {
    for (const auto& child : Children())
        if (auto* control = dynamic_cast<Control*>(child.get())) control->Update(deltaSeconds);
}

void Control::Draw(graphics::Renderer2D& renderer, const Theme& theme) {
    if (m_visibility != Visibility::Visible) return;
    DrawSelf(renderer, theme);
    for (const auto& child : Children()) {
        if (auto* control = dynamic_cast<Control*>(child.get())) control->Draw(renderer, theme);
    }
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
