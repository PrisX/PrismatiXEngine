#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Core/Variant.h"
#include "Engine/Scene/Node.h"
#include "Engine/UI/Styles/StyleDefinition.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

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
    using SignalArguments = VariantObject;
    using SignalHandler = std::function<void(const SignalArguments&)>;
    using SignalConnection = std::uint64_t;

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
    void SetStyleBinding(ControlStyleBinding value) { m_styleBinding = std::move(value); }
    [[nodiscard]] ControlStyleBinding& StyleBinding() { return m_styleBinding; }
    [[nodiscard]] const ControlStyleBinding& StyleBinding() const { return m_styleBinding; }
    void SetStyleToken(TokenRefValue value) { m_styleToken = std::move(value); }
    [[nodiscard]] const TokenRefValue& StyleToken() const { return m_styleToken; }
    void SetVisualChecked(bool value) { m_visualChecked = value; }
    void SetVisualSelected(bool value) { m_visualSelected = value; }
    [[nodiscard]] StyleStateSet ActiveStyleStates() const;
    void SetTooltip(std::string value) { m_tooltip = std::move(value); }
    [[nodiscard]] const std::string& Tooltip() const { return m_tooltip; }
    void SetPivot(Vec2 value) { m_pivot = value; }
    [[nodiscard]] Vec2 Pivot() const { return m_pivot; }
    void SetScale(Vec2 value) { m_scale = value; }
    [[nodiscard]] Vec2 Scale() const { return m_scale; }
    void SetRotation(float degrees) { m_rotation = degrees; }
    [[nodiscard]] float Rotation() const { return m_rotation; }
    void SetOpacity(float value) { m_opacity = std::clamp(value,0.0f,1.0f); }
    [[nodiscard]] float Opacity() const { return m_opacity; }
    void SetModulate(Color value) { m_modulate = value; }
    [[nodiscard]] Color Modulate() const { return m_modulate; }
    void SetClipContent(bool value) { m_clipContent = value; }
    [[nodiscard]] bool ClipContent() const { return m_clipContent; }
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
    void EmitInputSignal(const UIEvent& event);
    [[nodiscard]] SignalConnection ConnectSignal(std::string signal, SignalHandler handler);
    bool DisconnectSignal(std::string_view signal, SignalConnection connection);
    void EmitSignal(std::string_view signal, const SignalArguments& arguments = {});
    virtual void Update(float deltaSeconds);
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
    ControlStyleBinding m_styleBinding;
    TokenRefValue m_styleToken;
    bool m_visualChecked = false;
    bool m_visualSelected = false;
    std::string m_tooltip;
    Vec2 m_pivot{.5f,.5f};
    Vec2 m_scale{1,1};
    float m_rotation = 0.0f;
    float m_opacity = 1.0f;
    Color m_modulate{255,255,255,255};
    bool m_clipContent = false;
    struct SignalSlot {
        SignalConnection id = 0;
        SignalHandler handler;
    };
    std::unordered_map<std::string, std::vector<SignalSlot>> m_signalHandlers;
    SignalConnection m_nextSignalConnection = 1;
};

class Container : public Control {
public:
    explicit Container(std::string name = "Container") : Control(std::move(name)) {
        SetMouseFilter(MouseFilter::Pass);
    }
    [[nodiscard]] std::string_view TypeName() const override { return "Container"; }
};

}  // namespace px::ui
