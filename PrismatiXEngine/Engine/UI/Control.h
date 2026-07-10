#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Scene/Node.h"

#include <functional>
#include <limits>
#include <string>

namespace px::graphics { class Renderer2D; }

namespace px::ui {

class Theme;

enum class Visibility { Visible, Hidden, Collapsed };
enum class MouseFilter { Stop, Pass, Ignore };
enum class FocusMode { None, Click, All };
enum class SizeFlag : unsigned { ShrinkBegin = 0, Fill = 1, Expand = 2, ShrinkCenter = 4, ShrinkEnd = 8 };

// Describes who owns the placement of direct child Controls. The runtime and
// Designer share this policy so the editor never offers a transform that the
// layout engine will immediately overwrite.
enum class ChildLayoutPolicy {
    Free,
    LinearX,
    LinearY,
    Grid,
    Flow,
    SingleSlot,
    Pages,
    RuntimeManaged,
};

[[nodiscard]] const char* ChildLayoutPolicyName(ChildLayoutPolicy policy);

namespace ControlLayoutMath {
[[nodiscard]] Rect ResolveChildRect(Rect parent, Rect anchors, Rect offsets, Vec2 desired);
[[nodiscard]] Rect OffsetsForRect(Rect parent, Rect anchors, Rect child);
}

inline SizeFlag operator|(SizeFlag a, SizeFlag b) {
    return static_cast<SizeFlag>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
inline bool HasFlag(SizeFlag value, SizeFlag flag) {
    return (static_cast<unsigned>(value) & static_cast<unsigned>(flag)) != 0;
}

struct LayoutConstraints {
    Vec2 minimum{};
    Vec2 maximum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
};

enum class UIEventType {
    PointerEnter, PointerExit, PointerMove, PointerDown, PointerUp, Click, Scroll,
    FocusGained, FocusLost, KeyDown, TextInput, Activate, Cancel
};

struct UIEvent {
    UIEventType type = UIEventType::PointerMove;
    Vec2 position{};
    Vec2 delta{};
    float wheel = 0.0f;
    int key = 0;
    std::string text;
    bool handled = false;
    bool stopPropagation = false;
};

class Control : public scene::Node {
public:
    explicit Control(std::string name = "Control") : Node(std::move(name)) {}
    [[nodiscard]] std::string_view TypeName() const override { return "Control"; }
    [[nodiscard]] virtual ChildLayoutPolicy ChildPolicy() const {
        return ChildLayoutPolicy::Free;
    }

    void SetAnchors(Rect anchors);
    [[nodiscard]] Rect Anchors() const { return m_anchors; }
    void SetOffsets(Rect offsets);
    [[nodiscard]] Rect Offsets() const { return m_offsets; }
    void SetCustomMinimumSize(Vec2 value);
    [[nodiscard]] Vec2 CustomMinimumSize() const { return m_customMinimum; }
    void SetMaximumSize(Vec2 value);
    [[nodiscard]] Vec2 MaximumSize() const { return m_maximum; }
    void SetSizeFlags(SizeFlag horizontal, SizeFlag vertical);
    void SetStretchRatio(float value);

    void SetVisibility(Visibility value);
    [[nodiscard]] Visibility GetVisibility() const { return m_visibility; }
    [[nodiscard]] bool IsVisibleInTree() const;
    void SetEnabled(bool value) { m_enabled = value; }
    [[nodiscard]] bool Enabled() const { return m_enabled; }
    void SetMouseFilter(MouseFilter value) { m_mouseFilter = value; }
    [[nodiscard]] MouseFilter GetMouseFilter() const { return m_mouseFilter; }
    void SetFocusMode(FocusMode value) { m_focusMode = value; }
    [[nodiscard]] FocusMode GetFocusMode() const { return m_focusMode; }
    void SetThemeVariant(std::string value) { m_themeVariant = std::move(value); }
    [[nodiscard]] const std::string& ThemeVariant() const { return m_themeVariant; }
    void SetTooltip(std::string value) { m_tooltip = std::move(value); }
    [[nodiscard]] const std::string& Tooltip() const { return m_tooltip; }

    [[nodiscard]] Vec2 Measure(Vec2 available);
    void Arrange(Rect finalRect);
    [[nodiscard]] Rect LayoutRect() const { return m_layoutRect; }
    [[nodiscard]] Vec2 DesiredSize() const { return m_desiredSize; }
    [[nodiscard]] SizeFlag HorizontalSizeFlags() const { return m_horizontalFlags; }
    [[nodiscard]] SizeFlag VerticalSizeFlags() const { return m_verticalFlags; }
    [[nodiscard]] float StretchRatio() const { return m_stretchRatio; }
    void InvalidateLayout();
    [[nodiscard]] bool LayoutDirty() const { return m_layoutDirty; }

    [[nodiscard]] virtual bool HitTest(Vec2 point) const;
    virtual void HandleEvent(UIEvent& event);
    virtual void Draw(graphics::Renderer2D& renderer, const Theme& theme);

    [[nodiscard]] bool Hovered() const { return m_hovered; }
    [[nodiscard]] bool Pressed() const { return m_pressed; }
    [[nodiscard]] bool Focused() const { return m_focused; }
    void SetInteractionState(bool hovered, bool pressed, bool focused);

protected:
    [[nodiscard]] virtual Vec2 MeasureOverride(Vec2 available);
    virtual void ArrangeOverride(Rect finalRect);
    virtual void DrawSelf(graphics::Renderer2D& renderer, const Theme& theme);

private:
    Rect m_anchors{0, 0, 0, 0};
    Rect m_offsets{0, 0, 0, 0};
    Vec2 m_customMinimum{};
    Vec2 m_maximum{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Rect m_layoutRect{};
    Vec2 m_desiredSize{};
    SizeFlag m_horizontalFlags = SizeFlag::Fill;
    SizeFlag m_verticalFlags = SizeFlag::Fill;
    float m_stretchRatio = 1.0f;
    Visibility m_visibility = Visibility::Visible;
    MouseFilter m_mouseFilter = MouseFilter::Stop;
    FocusMode m_focusMode = FocusMode::None;
    bool m_enabled = true;
    bool m_layoutDirty = true;
    bool m_hovered = false;
    bool m_pressed = false;
    bool m_focused = false;
    std::string m_themeVariant = "Default";
    std::string m_tooltip;
};

class Container : public Control {
public:
    explicit Container(std::string name = "Container") : Control(std::move(name)) {}
    [[nodiscard]] std::string_view TypeName() const override { return "Container"; }
};

}  // namespace px::ui
