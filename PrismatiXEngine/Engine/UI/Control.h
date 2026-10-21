#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Core/Variant.h"
#include "Engine/Scene/Node.h"
#include "Engine/Text/TextLayout.h"
#include "Engine/UI/Styles/StyleDefinition.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::graphics { class Renderer2D; }

namespace px::ui {

class Theme;

// A panel drawn behind a control's own content: rounded corners, a border,
// a two-colour gradient and a soft drop shadow, all without a texture.
struct BoxDecoration {
    Color fill{0, 0, 0, 0};
    std::optional<Color> fillEnd;
    bool horizontal = false;
    std::optional<Color> hoverFill;
    float radius = 0.0f;
    float borderWidth = 0.0f;
    Color border{0, 0, 0, 0};
    std::optional<Color> hoverBorder;
    // Caption colour on hover, for buttons that draw text.
    std::optional<Color> hoverText;
    Color shadow{0, 0, 0, 0};
    float shadowBlur = 0.0f;
    Vec2 shadowOffset{};
    // Alpha multiplier while the control is disabled.
    float disabledOpacity = 1.0f;
};

enum class Visibility { Visible, Hidden, Collapsed };
// Curve of a show/hide slide. Back overshoots its rest on show and pulls back
// before leaving on hide, like a spring.
enum class TransitionEase { Quad, Linear, Cubic, Back };
// A show wipe reveals the control from that edge (Left: left to right); a
// hide wipe takes it away towards that edge.
enum class TransitionWipe { None, Left, Right, Up, Down };

[[nodiscard]] const char* TransitionWipeName(TransitionWipe wipe);
[[nodiscard]] std::optional<TransitionWipe> ParseTransitionWipe(std::string_view value);
[[nodiscard]] const char* TransitionEaseName(TransitionEase ease);
[[nodiscard]] std::optional<TransitionEase> ParseTransitionEase(std::string_view value);

// Everything a show transition does, for applying one to runtime-made
// controls (list and grid items).
struct ShowTransitionSpec {
    float seconds = 0.0f;
    float delaySeconds = 0.0f;
    float offsetX = 0.0f;
    float offsetY = 8.0f;
    TransitionEase ease = TransitionEase::Quad;
    TransitionWipe wipe = TransitionWipe::None;
    float scale = 1.0f;
    bool fade = true;
};
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
    std::string text{};
    bool shift = false;
    bool control = false;
    bool handled = false;
    bool stopPropagation = false;
};

// Backend-neutral accessibility description. Platform adapters and the CI
// mock adapter consume this same semantic data instead of rediscovering UI
// meaning from pixels or control implementation details.
struct AccessibilityTextSemantics {
    text::TextLayout layout;
    Vec2 origin{};
    std::size_t caretByteOffset = 0;
    std::size_t selectionStartByteOffset = 0;
    std::size_t selectionEndByteOffset = 0;
    bool editable = false;
    bool multiline = false;
};

struct AccessibilitySemantics {
    Uuid id;
    std::string role{};
    std::string label{};
    std::string value{};
    std::string description{};
    std::vector<std::string> states{};
    std::vector<std::string> actions{};
    Rect bounds{};
    double minimum = 0.0;
    double maximum = 0.0;
    double step = 0.0;
    bool hasRange = false;
    bool readOnly = false;
    std::int32_t focusOrder = 0;
    bool focusable = false;
    bool hidden = false;
    std::optional<AccessibilityTextSemantics> text{};
};

class Control : public scene::Node {
public:
    using TextLayoutProvider = std::function<text::TextLayout(
        const Control&, std::string_view, int, int, text::TextOrientation,
        std::size_t)>;
    // Mixed-font paragraphs; a run whose font path is empty or only a style
    // suffix ("|b") uses the control's theme font.
    using TextRunsLayoutProvider = std::function<text::TextLayout(
        const Control&, std::string_view, const std::vector<text::TextFontRun>&, int)>;
    using SignalArguments = VariantObject;
    using SignalHandler = std::function<void(const SignalArguments&)>;
    using SignalConnection = std::uint64_t;

    explicit Control(std::string name = "Control") : Node(std::move(name)) {}
    [[nodiscard]] std::string_view TypeName() const override { return "Control"; }
    [[nodiscard]] virtual ChildLayoutPolicy ChildPolicy() const {
        return ChildLayoutPolicy::Free;
    }
    // Reports image paths resolved on this mounted control. Custom controls
    // can participate in route preloading without exposing render internals.
    virtual void CollectImageDependencies(std::vector<std::string>&) const {}

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
    void SetModalCapture(bool value) { m_modalCapture = value; }
    [[nodiscard]] bool ModalCapture() const { return m_modalCapture; }
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
    void SetHoverSoundPath(std::string value) { m_hoverSoundPath = std::move(value); }
    [[nodiscard]] const std::string& HoverSoundPath() const { return m_hoverSoundPath; }
    void SetActivateSoundPath(std::string value) { m_activateSoundPath = std::move(value); }
    [[nodiscard]] const std::string& ActivateSoundPath() const { return m_activateSoundPath; }
    // A sound the control plays as it appears: when its show transition
    // starts (after its delay and any stagger, once every parent has begun
    // to show), or at once without one, and `delaySeconds` after that. The
    // hide sound plays as it starts to hide. Variants, when given, replace
    // the path; one is picked each time.
    struct Sound {
        std::string path;
        std::vector<std::string> variants;
        float volume = 1.0f;
        float delaySeconds = 0.0f;
        [[nodiscard]] bool Empty() const { return path.empty() && variants.empty(); }
    };
    [[nodiscard]] Sound& EditShowSound() { return m_showSound; }
    [[nodiscard]] const Sound& ShowSound() const { return m_showSound; }
    [[nodiscard]] Sound& EditHideSound() { return m_hideSound; }
    [[nodiscard]] const Sound& HideSound() const { return m_hideSound; }
    // Seconds the control has been visible since it was last shown.
    [[nodiscard]] float VisibleSeconds() const { return m_visibleSeconds; }
    void SetPivot(Vec2 value) { m_pivot = value; }
    [[nodiscard]] Vec2 Pivot() const { return m_pivot; }
    void SetScale(Vec2 value) { m_scale = value; }
    [[nodiscard]] Vec2 Scale() const { return m_scale; }
    void SetTranslation(Vec2 value) { m_translation = value; }
    [[nodiscard]] Vec2 Translation() const { return m_translation; }
    void SetRotation(float degrees) { m_rotation = degrees; }
    [[nodiscard]] float Rotation() const { return m_rotation; }
    void SetOpacity(float value) { m_opacity = std::clamp(value,0.0f,1.0f); }
    [[nodiscard]] float Opacity() const { return m_opacity; }
    void SetShowTransitionSeconds(float value) {
        m_showTransitionSeconds = std::clamp(value, 0.0f, 2.0f);
    }
    [[nodiscard]] float ShowTransitionSeconds() const { return m_showTransitionSeconds; }
    // Leaving Visible keeps drawing the control for this long, fading it out
    // and sliding it by HideOffsetY pixels. Zero hides it at once.
    void SetHideTransitionSeconds(float value) {
        m_hideTransitionSeconds = std::clamp(value, 0.0f, 2.0f);
    }
    [[nodiscard]] float HideTransitionSeconds() const { return m_hideTransitionSeconds; }
    // Vertical distance the control travels while it appears (it starts that
    // far from its place) or disappears (it ends that far from it).
    void SetShowOffsetY(float value) { m_showOffsetY = std::clamp(value, -2000.0f, 2000.0f); }
    [[nodiscard]] float ShowOffsetY() const { return m_showOffsetY; }
    void SetHideOffsetY(float value) { m_hideOffsetY = std::clamp(value, -2000.0f, 2000.0f); }
    [[nodiscard]] float HideOffsetY() const { return m_hideOffsetY; }
    void SetShowOffsetX(float value) { m_showOffsetX = std::clamp(value, -2000.0f, 2000.0f); }
    [[nodiscard]] float ShowOffsetX() const { return m_showOffsetX; }
    void SetHideOffsetX(float value) { m_hideOffsetX = std::clamp(value, -2000.0f, 2000.0f); }
    [[nodiscard]] float HideOffsetX() const { return m_hideOffsetX; }
    // Linear progress instead of the default squared ease-out.
    void SetLinearTransition(bool value) { m_linearTransition = value; }
    [[nodiscard]] bool LinearTransition() const { return m_linearTransition; }
    void SetShowEase(TransitionEase value) { m_showEase = value; }
    [[nodiscard]] TransitionEase ShowEase() const { return m_showEase; }
    void SetHideEase(TransitionEase value) { m_hideEase = value; }
    [[nodiscard]] TransitionEase HideEase() const { return m_hideEase; }
    // Seconds the control waits, unseen, before its show transition starts.
    void SetShowDelaySeconds(float value) {
        m_showDelaySeconds = std::isfinite(value) ? std::clamp(value, 0.0f, 10.0f) : 0.0f;
    }
    [[nodiscard]] float ShowDelaySeconds() const { return m_showDelaySeconds; }
    void SetShowWipe(TransitionWipe value) { m_showWipe = value; }
    [[nodiscard]] TransitionWipe ShowWipe() const { return m_showWipe; }
    void SetHideWipe(TransitionWipe value) { m_hideWipe = value; }
    [[nodiscard]] TransitionWipe HideWipe() const { return m_hideWipe; }
    // Scale the control grows (or shrinks) from as it appears; 1 is none.
    void SetShowScale(float value) { m_showScale = std::isfinite(value) ? std::clamp(value, 0.0f, 10.0f) : 1.0f; }
    [[nodiscard]] float ShowScale() const { return m_showScale; }
    // Scale the control ends at as it disappears; 1 is none.
    void SetHideScale(float value) { m_hideScale = std::isfinite(value) ? std::clamp(value, 0.0f, 10.0f) : 1.0f; }
    [[nodiscard]] float HideScale() const { return m_hideScale; }
    // Whether the show/hide transition fades the opacity (on by default).
    void SetShowFade(bool value) { m_showFade = value; }
    [[nodiscard]] bool ShowFade() const { return m_showFade; }
    void SetHideFade(bool value) { m_hideFade = value; }
    [[nodiscard]] bool HideFade() const { return m_hideFade; }
    // Each child's show transition starts this much later than the previous
    // visible child's (list and grid items in their order).
    void SetStaggerSeconds(float value) {
        m_staggerSeconds = std::isfinite(value) ? std::clamp(value, 0.0f, 2.0f) : 0.0f;
    }
    [[nodiscard]] float StaggerSeconds() const { return m_staggerSeconds; }
    // The delay a staggering parent gives this control.
    void SetStaggerDelaySeconds(float value) {
        m_staggerDelaySeconds = std::isfinite(value) ? std::clamp(value, 0.0f, 60.0f) : 0.0f;
    }
    [[nodiscard]] float StaggerDelaySeconds() const { return m_staggerDelaySeconds; }
    // Scale the control eases to while the pointer is over it; 1 is none.
    void SetHoverScale(float value) { m_hoverScale = std::isfinite(value) ? std::clamp(value, 0.1f, 4.0f) : 1.0f; }
    [[nodiscard]] float HoverScale() const { return m_hoverScale; }
    void SetHoverTransitionSeconds(float value) {
        m_hoverTransitionSeconds = std::isfinite(value) ? std::clamp(value, 0.0f, 2.0f) : 0.12f;
    }
    [[nodiscard]] float HoverTransitionSeconds() const { return m_hoverTransitionSeconds; }
    void ApplyShowTransition(const ShowTransitionSpec& spec);
    // Starts the show transition again from its delay, as when it appeared.
    void RestartShowTransition();
    // Ends any show transition at once (the control is fully shown).
    void FinishShowTransition();
    // Whether this control and every parent have begun to show: their delays
    // and staggers have passed.
    [[nodiscard]] bool ShowStartedInTree() const;
    [[nodiscard]] float ShowTotalSeconds() const {
        return m_showDelaySeconds + m_staggerDelaySeconds + m_showTransitionSeconds;
    }
    void SetModulate(Color value) { m_modulate = value; }
    [[nodiscard]] Color Modulate() const { return m_modulate; }
    void SetClipContent(bool value) { m_clipContent = value; }
    [[nodiscard]] bool ClipContent() const { return m_clipContent; }
    void SetAccessibilityLabel(std::string value) { m_accessibilityLabel = std::move(value); }
    [[nodiscard]] const std::string& AccessibilityLabel() const { return m_accessibilityLabel; }
    void SetAccessibilityRole(std::string value) { m_accessibilityRole = std::move(value); }
    [[nodiscard]] const std::string& AccessibilityRole() const { return m_accessibilityRole; }
    void SetAccessibilityDescription(std::string value) { m_accessibilityDescription = std::move(value); }
    [[nodiscard]] const std::string& AccessibilityDescription() const { return m_accessibilityDescription; }
    void SetAccessibilityFocusOrder(std::int32_t value) { m_accessibilityFocusOrder = value; }
    [[nodiscard]] std::int32_t AccessibilityFocusOrder() const { return m_accessibilityFocusOrder; }
    void SetTextLayoutProvider(TextLayoutProvider provider) {
        m_textLayoutProvider = std::move(provider);
        InvalidateLayout();
    }
    void SetTextRunsLayoutProvider(TextRunsLayoutProvider provider) {
        m_textRunsLayoutProvider = std::move(provider);
        InvalidateLayout();
    }
    // Custom controls may override this to expose domain-specific value,
    // state, and actions while retaining the shared semantic-tree lifecycle.
    [[nodiscard]] virtual AccessibilitySemantics DescribeAccessibility() const;
    virtual bool PerformAccessibilityAction(std::string_view action,
                                            std::string_view value = {});
    // Text controls capture directional and Accept bindings before global
    // focus navigation so ordinary typing cannot move focus or submit on Space.
    [[nodiscard]] virtual bool CapturesTextInput() const { return false; }
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
    [[nodiscard]] bool HasSignalHandlers(std::string_view signal) const;
    void EmitSignal(std::string_view signal, const SignalArguments& arguments = {});
    virtual void Update(float deltaSeconds);
    virtual void Draw(graphics::Renderer2D& renderer, const Theme& theme);
    void SetBoxDecoration(std::optional<BoxDecoration> decoration) {
        m_box = std::move(decoration);
    }
    [[nodiscard]] const std::optional<BoxDecoration>& GetBoxDecoration() const { return m_box; }

    [[nodiscard]] bool Hovered() const { return m_hovered; }
    [[nodiscard]] bool Pressed() const { return m_pressed; }
    [[nodiscard]] bool Focused() const { return m_focused; }
    // Focused, and the focus should be drawn: the last input was not the
    // pointer, so a clicked control does not keep its hover look.
    [[nodiscard]] bool FocusVisible() const { return m_focused && m_focusVisible; }
    void SetInteractionState(bool hovered, bool pressed, bool focused,
                             bool focusVisible = true);

protected:
    [[nodiscard]] std::optional<text::TextLayout> MeasureTextLayout(
        std::string_view value, int size, int wrap = 0,
        text::TextOrientation orientation = text::TextOrientation::Horizontal,
        std::size_t verticalRows = 0) const {
        if (!m_textLayoutProvider) return std::nullopt;
        return m_textLayoutProvider(*this, value, size, wrap, orientation,
                                    verticalRows);
    }
    [[nodiscard]] std::optional<text::TextLayout> MeasureTextRuns(
        std::string_view value, const std::vector<text::TextFontRun>& runs,
        int wrap = 0) const {
        if (!m_textRunsLayoutProvider) return std::nullopt;
        return m_textRunsLayoutProvider(*this, value, runs, wrap);
    }
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
    bool m_modalCapture = false;
    FocusMode m_focusMode = FocusMode::None;
    bool m_enabled = true;
    bool m_layoutDirty = true;
    bool m_hovered = false;
    bool m_pressed = false;
    bool m_focused = false;
    bool m_focusVisible = true;
    ControlStyleBinding m_styleBinding;
    TokenRefValue m_styleToken;
    bool m_visualChecked = false;
    bool m_visualSelected = false;
    std::string m_tooltip;
    std::string m_hoverSoundPath;
    std::string m_activateSoundPath;
    Sound m_showSound;
    Sound m_hideSound;
    // The show/hide has begun and its "showSound"/"hideSound" signal is
    // still to be emitted; m_showSoundWait counts the show sound's delay from
    // the moment the control began to show.
    bool m_showSignalPending = true;
    bool m_hideSignalPending = false;
    float m_showSoundWait = -1.0f;
    float m_visibleSeconds = 0.0f;
    Vec2 m_pivot{.5f,.5f};
    Vec2 m_scale{1,1};
    Vec2 m_translation{};
    float m_rotation = 0.0f;
    float m_opacity = 1.0f;
    float m_showTransitionSeconds = 0.0f;
    float m_showElapsed = 0.0f;
    float m_hideTransitionSeconds = 0.0f;
    float m_hideElapsed = 0.0f;
    bool m_hiding = false;
    bool m_presented = false;
    float m_showOffsetY = 8.0f;
    float m_hideOffsetY = 0.0f;
    float m_showOffsetX = 0.0f;
    float m_hideOffsetX = 0.0f;
    bool m_linearTransition = false;
    float m_showDelaySeconds = 0.0f;
    float m_staggerDelaySeconds = 0.0f;
    float m_staggerSeconds = 0.0f;
    TransitionWipe m_showWipe = TransitionWipe::None;
    TransitionWipe m_hideWipe = TransitionWipe::None;
    float m_showScale = 1.0f;
    float m_hideScale = 1.0f;
    bool m_showFade = true;
    bool m_hideFade = true;
    float m_hoverScale = 1.0f;
    float m_hoverTransitionSeconds = 0.12f;
    float m_hoverAmount = 0.0f;
    TransitionEase m_showEase = TransitionEase::Quad;
    TransitionEase m_hideEase = TransitionEase::Quad;
    Color m_modulate{255,255,255,255};
    bool m_clipContent = false;
    std::optional<BoxDecoration> m_box;
    std::string m_accessibilityLabel;
    std::string m_accessibilityRole;
    std::string m_accessibilityDescription;
    std::int32_t m_accessibilityFocusOrder = 0;
    TextLayoutProvider m_textLayoutProvider;
    TextRunsLayoutProvider m_textRunsLayoutProvider;
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
