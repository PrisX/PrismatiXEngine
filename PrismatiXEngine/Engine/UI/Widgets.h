#pragma once
#include "Engine/UI/Actions/ActionDescriptor.h"

#include "Engine/UI/Control.h"
#include "Engine/Graphics/Texture.h"
#include "Engine/SDK/Motion.h"
#include "Engine/SDK/Portrait.h"
#include "Engine/Text/Typography.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace px { class Input; }

namespace px::ui {

enum class HorizontalTextAlignment { Left, Center, Right };
enum class VerticalTextAlignment { Top, Center, Bottom };
enum class TextureScaleMode { Stretch, Fit, Fill, Original };
enum class BindingDevice { Keyboard, Wheel };

class Panel : public Control {
public:
    explicit Panel(std::string name = "Panel") : Control(std::move(name)) {
        SetMouseFilter(MouseFilter::Pass);
    }
    [[nodiscard]] std::string_view TypeName() const override { return "Panel"; }
protected:
    Vec2 MeasureOverride(Vec2 available) override;
    void ArrangeOverride(Rect finalRect) override;
    void DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) override;
};

class Label : public Control {
public:
    explicit Label(std::string text = {}, std::string name = "Label") : Control(std::move(name)), m_text(std::move(text)) {
        SetMouseFilter(MouseFilter::Ignore);
    }
    [[nodiscard]] std::string_view TypeName() const override { return "Label"; }
    void SetText(std::string value) { if (m_text == value) return; m_text = std::move(value); InvalidateLayout(); }
    [[nodiscard]] const std::string& Text() const { return m_text; }
    void SetWrap(bool value) { if (m_wrap == value) return; m_wrap = value; InvalidateLayout(); }
    [[nodiscard]] bool Wrap() const { return m_wrap; }
    void SetFontSize(int value) { if (m_fontSize == value) return; m_fontSize = value; InvalidateLayout(); }
    [[nodiscard]] int FontSize() const { return m_fontSize; }
    // A VFS font file for this label only; empty uses the theme font.
    void SetFontPath(std::string value) { m_fontPath = std::move(value); }
    [[nodiscard]] const std::string& FontPath() const { return m_fontPath; }
    // Extra pixels between characters of a single-line label.
    void SetLetterSpacing(float value) {
        m_letterSpacing = std::isfinite(value) ? std::clamp(value, -64.0f, 64.0f) : 0.0f;
    }
    [[nodiscard]] float LetterSpacing() const { return m_letterSpacing; }
    [[nodiscard]] int FontSizeToFit(std::string_view content, int requested,
                                    float width, float height) const;
    void SetColor(Color value) { m_color = value; m_hasColor = true; }
    [[nodiscard]] Color GetColor() const { return m_color; }
    [[nodiscard]] bool HasColor() const { return m_hasColor; }
    void SetOutlineSize(int value) { m_outlineSize = std::clamp(value, 0, 8); }
    [[nodiscard]] int OutlineSize() const { return m_outlineSize; }
    void SetOutlineColor(Color value) { m_outlineColor = value; }
    [[nodiscard]] Color OutlineColor() const { return m_outlineColor; }
    void SetHorizontalAlignment(HorizontalTextAlignment value) { m_horizontalAlignment=value; }
    void SetVerticalAlignment(VerticalTextAlignment value) { m_verticalAlignment=value; }
    [[nodiscard]] HorizontalTextAlignment HorizontalAlignment() const { return m_horizontalAlignment; }
    [[nodiscard]] VerticalTextAlignment VerticalAlignment() const { return m_verticalAlignment; }
    // The whole line while a typewriter shows a prefix of it: wrapping is
    // decided on the whole line, so characters never jump to the next line
    // as they appear.
    void SetLayoutReference(std::string value) { m_layoutReference = std::move(value); }

protected:
    [[nodiscard]] const std::string& LayoutReference() const { return m_layoutReference; }
    Vec2 MeasureOverride(Vec2 available) override;
    void DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) override;
    // Wrapped text with line breaks at UAX #14 opportunities and 禁則.
    [[nodiscard]] std::string ReflowedText(graphics::Renderer2D& renderer, const std::string& font,
                                           int size, float width) const;

private:
    std::string m_layoutReference;
    mutable std::string m_reflowKey;
    mutable std::vector<std::size_t> m_reflowBreaks;
    std::string m_text;
    bool m_wrap = false;
    int m_fontSize = 0;
    std::string m_fontPath;
    float m_letterSpacing = 0.0f;
    Color m_color{};
    bool m_hasColor = false;
    int m_outlineSize = 0;
    Color m_outlineColor{0, 0, 0, 255};
    HorizontalTextAlignment m_horizontalAlignment = HorizontalTextAlignment::Left;
    VerticalTextAlignment m_verticalAlignment = VerticalTextAlignment::Top;
};

class Button : public Control {
public:
    using Activated = std::function<void()>;
    explicit Button(std::string text = {}, std::string name = "Button");
    ~Button() override;
    [[nodiscard]] std::string_view TypeName() const override { return "Button"; }
    void SetText(std::string value) { if (m_text == value) return; m_text = std::move(value); InvalidateLayout(); }
    [[nodiscard]] const std::string& Text() const { return m_text; }
    void SetFontSize(int value) { if (m_fontSize == value) return; m_fontSize = value; InvalidateLayout(); }
    [[nodiscard]] int FontSize() const { return m_fontSize; }
    void SetTextColor(Color value) { m_textColor = value; m_hasTextColor = true; }
    void ClearTextColor() { m_hasTextColor = false; }
    // A project font file for the caption; empty uses the theme font.
    void SetFontPath(std::string value) { m_fontPath = std::move(value); InvalidateLayout(); }
    [[nodiscard]] const std::string& FontPath() const { return m_fontPath; }
    [[nodiscard]] Color GetTextColor() const { return m_textColor; }
    [[nodiscard]] bool HasTextColor() const { return m_hasTextColor; }
    void SetOnActivated(Activated value) { m_activated = std::move(value); }
    // Authored onClick action id, kept so hosts can choose default feedback
    // (for example a cancel sound) without parsing the UI document again.
    void SetActivationActionId(std::string value) { m_activationActionId = std::move(value); }
    [[nodiscard]] const std::string& ActivationActionId() const { return m_activationActionId; }
    void SetBindingAction(std::string action);
    [[nodiscard]] const std::string& BindingAction() const { return m_bindingAction; }
    void SetBindingDevice(BindingDevice device);
    [[nodiscard]] BindingDevice GetBindingDevice() const { return m_bindingDevice; }
    [[nodiscard]] bool SetWheelThreshold(float threshold);
    [[nodiscard]] float WheelThreshold() const { return m_wheelThreshold; }
    void SetWheelUpText(std::string value) { m_wheelUpText = std::move(value); RefreshBindingText(); }
    [[nodiscard]] const std::string& WheelUpText() const { return m_wheelUpText; }
    void SetWheelDownText(std::string value) { m_wheelDownText = std::move(value); RefreshBindingText(); }
    [[nodiscard]] const std::string& WheelDownText() const { return m_wheelDownText; }
    void SetBindingPrompt(std::string value) { m_bindingPrompt = std::move(value); }
    [[nodiscard]] const std::string& BindingPrompt() const { return m_bindingPrompt; }
    void SetUnboundText(std::string value) { m_unboundText = std::move(value); }
    [[nodiscard]] const std::string& UnboundText() const { return m_unboundText; }
    void SetBindingConflictText(std::string value) { m_bindingConflictText = std::move(value); }
    [[nodiscard]] const std::string& BindingConflictText() const { return m_bindingConflictText; }
    void SetBindingConfirmText(std::string value) { m_bindingConfirmText = std::move(value); }
    [[nodiscard]] const std::string& BindingConfirmText() const { return m_bindingConfirmText; }
    void SetBindingLimitText(std::string value) { m_bindingLimitText = std::move(value); }
    [[nodiscard]] const std::string& BindingLimitText() const { return m_bindingLimitText; }
    void SetEscapeCancelsBinding(bool value) { m_escapeCancelsBinding = value; }
    [[nodiscard]] bool EscapeCancelsBinding() const { return m_escapeCancelsBinding; }
    void SetAllowSharedBinding(bool value) { m_allowSharedBinding = value; }
    [[nodiscard]] bool AllowSharedBinding() const { return m_allowSharedBinding; }
    void SetConfirmSharedBinding(bool value) { m_confirmSharedBinding = value; }
    [[nodiscard]] bool ConfirmSharedBinding() const { return m_confirmSharedBinding; }
    void SetDeleteClearsBinding(bool value) { m_deleteClearsBinding = value; }
    [[nodiscard]] bool DeleteClearsBinding() const { return m_deleteClearsBinding; }
    void SetAddCapturedBinding(bool value) { m_addCapturedBinding = value; }
    [[nodiscard]] bool AddCapturedBinding() const { return m_addCapturedBinding; }
    void SetSelectableBindingKeys(bool value) {
        m_selectableBindingKeys = value;
        m_selectedBindingIndex = 0;
        RefreshBindingText();
    }
    [[nodiscard]] bool SelectableBindingKeys() const {
        return m_selectableBindingKeys;
    }
    [[nodiscard]] std::size_t SelectedBindingIndex() const {
        return m_selectedBindingIndex;
    }
    // Non-owning input service; UIContext reapplies it after every root swap.
    void SetInputBindingService(Input* input, std::function<void()> onChanged = {});
    [[nodiscard]] bool CapturingBinding() const { return m_capturingBinding; }
    [[nodiscard]] bool CapturingWheelBinding() const { return m_capturingWheelBinding; }
    void SetActionInvocation(ActionInvocation value) { m_action = std::move(value); }
    [[nodiscard]] const ActionInvocation& BoundAction() const { return m_action; }
    void SetHorizontalAlignment(HorizontalTextAlignment value) { m_horizontalAlignment=value; }
    void SetVerticalAlignment(VerticalTextAlignment value) { m_verticalAlignment=value; }
    [[nodiscard]] HorizontalTextAlignment HorizontalAlignment() const { return m_horizontalAlignment; }
    [[nodiscard]] VerticalTextAlignment VerticalAlignment() const { return m_verticalAlignment; }
    void Activate();
    void HandleEvent(UIEvent& event) override;
    void Update(float deltaSeconds) override;

protected:
    Vec2 MeasureOverride(Vec2 available) override;
    void DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) override;
    // Image-backed subclasses still need the normal Button caption for
    // dynamic list rows and skinned settings controls. Keeping caption
    // rendering here avoids painting the default button chrome twice.
    void DrawTextContent(graphics::Renderer2D& renderer,
                         const Theme& theme) const;

private:
    struct PendingSharedBinding {
        int key = 0;
        std::optional<int> replacingKey;
        bool add = false;
    };
    void ApplyBindingKey(int key, std::optional<int> replacingKey, bool add,
                         bool allowConflict);
    void RefreshBindingText();
    std::string m_text;
    std::string m_bindingAction;
    BindingDevice m_bindingDevice = BindingDevice::Keyboard;
    float m_wheelThreshold = 0.0f;
    std::string m_wheelUpText = "Wheel Up";
    std::string m_wheelDownText = "Wheel Down";
    std::string m_bindingPrompt = "Press a key";
    std::string m_unboundText = "Unbound";
    std::string m_bindingConflictText = "Already assigned";
    std::string m_bindingConfirmText = "Already assigned - activate again to share";
    std::string m_bindingLimitText = "Too many keys";
    Input* m_bindingInput = nullptr;
    std::function<void()> m_bindingChanged;
    float m_bindingFeedbackSeconds = 0.0f;
    bool m_capturingBinding = false;
    bool m_capturingWheelBinding = false;
    bool m_escapeCancelsBinding = true;
    bool m_allowSharedBinding = false;
    bool m_confirmSharedBinding = false;
    bool m_deleteClearsBinding = false;
    bool m_addCapturedBinding = false;
    bool m_selectableBindingKeys = false;
    std::size_t m_selectedBindingIndex = 0;
    std::optional<int> m_replacingBindingKey;
    std::optional<PendingSharedBinding> m_pendingSharedBinding;
    int m_fontSize = 0;
    std::string m_fontPath;
    Color m_textColor{};
    bool m_hasTextColor = false;
    ActionInvocation m_action;
    Activated m_activated;
    std::string m_activationActionId;
    HorizontalTextAlignment m_horizontalAlignment = HorizontalTextAlignment::Center;
    VerticalTextAlignment m_verticalAlignment = VerticalTextAlignment::Center;
};

// A compact button that keeps the normal button interaction and actions but
// renders only its glyph. It is intended for HUD toolbars without button chrome.
class IconButton final : public Button {
public:
    explicit IconButton(std::string icon = {}, std::string name = "IconButton") : Button(std::move(icon), std::move(name)) {}
    [[nodiscard]] std::string_view TypeName() const override { return "IconButton"; }
protected:
    Vec2 MeasureOverride(Vec2 available) override;
    void DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) override;
};

// Image-backed button used by commercial VN interfaces.  Keeping all four
// interaction textures on one control preserves authored hit testing,
// keyboard focus, accessibility, and Actions without requiring a behavior
// graph just to swap sibling TextureRect visibility on hover/press.
class TextureButton final : public Button {
public:
    explicit TextureButton(std::string name = "TextureButton")
        : Button({}, std::move(name)) {}
    [[nodiscard]] std::string_view TypeName() const override {
        return "TextureButton";
    }
    void SetNormalPath(std::string value) { m_normalPath = std::move(value); }
    void SetHoverPath(std::string value) { m_hoverPath = std::move(value); }
    void SetPressedPath(std::string value) { m_pressedPath = std::move(value); }
    void SetDisabledPath(std::string value) { m_disabledPath = std::move(value); }
    [[nodiscard]] const std::string& NormalPath() const { return m_normalPath; }
    [[nodiscard]] const std::string& HoverPath() const { return m_hoverPath; }
    [[nodiscard]] const std::string& PressedPath() const { return m_pressedPath; }
    [[nodiscard]] const std::string& DisabledPath() const { return m_disabledPath; }
    void CollectImageDependencies(std::vector<std::string>& paths) const override;
    void SetScaleMode(TextureScaleMode value) { m_scaleMode = value; }
    [[nodiscard]] TextureScaleMode ScaleMode() const { return m_scaleMode; }
    // Non-zero margins (image pixels: left, top, right, bottom) draw every
    // state image as a nine-slice: corners keep their size (times
    // PatchScale), edges and centre stretch to the button.
    void SetPatchMargins(Rect value) { m_patchMargins = value; }
    [[nodiscard]] Rect PatchMargins() const { return m_patchMargins; }
    void SetPatchScale(float value) { m_patchScale = value; }
    [[nodiscard]] float PatchScale() const { return m_patchScale; }
    [[nodiscard]] bool NineSlice() const {
        return m_patchMargins.x > 0 || m_patchMargins.y > 0 || m_patchMargins.w > 0 || m_patchMargins.h > 0;
    }

protected:
    void DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) override;

private:
    std::string m_normalPath;
    std::string m_hoverPath;
    std::string m_pressedPath;
    std::string m_disabledPath;
    TextureScaleMode m_scaleMode = TextureScaleMode::Stretch;
    Rect m_patchMargins{0, 0, 0, 0};
    float m_patchScale = 1.0f;
};

class TextureRect : public Control {
public:
    using SourceLoader = std::function<std::optional<std::string>(std::string_view)>;
    explicit TextureRect(std::string path = {}, std::string name = "TextureRect")
        : Control(std::move(name)), m_path(std::move(path)) { SetMouseFilter(MouseFilter::Ignore); }
    [[nodiscard]] std::string_view TypeName() const override { return "TextureRect"; }
    Status SetPath(std::string value);
    Status SetSourceLoader(SourceLoader loader);
    [[nodiscard]] const std::string& Path() const { return m_path; }
    void CollectImageDependencies(std::vector<std::string>& paths) const override;
    Status SetTexture(ResourceRefValue value);
    [[nodiscard]] const ResourceRefValue& Texture() const { return m_texture; }
    void SetOpacity(float value);
    [[nodiscard]] float Opacity() const { return m_opacity; }
    void SetScaleMode(TextureScaleMode value) { m_scaleMode=value; }
    [[nodiscard]] TextureScaleMode ScaleMode() const { return m_scaleMode; }
    void SetHorizontalAlignment(HorizontalTextAlignment value) { m_horizontalAlignment=value; }
    void SetVerticalAlignment(VerticalTextAlignment value) { m_verticalAlignment=value; }
    [[nodiscard]] HorizontalTextAlignment HorizontalAlignment() const { return m_horizontalAlignment; }
    [[nodiscard]] VerticalTextAlignment VerticalAlignment() const { return m_verticalAlignment; }
    void SetLockAspectRatio(bool value) { m_lockAspectRatio=value; }
    [[nodiscard]] bool LockAspectRatio() const { return m_lockAspectRatio; }
    // A sub-rectangle of the image in source pixels (an atlas cell); an empty
    // rectangle shows the whole image.
    void SetRegion(Rect value) { m_region = value; }
    [[nodiscard]] Rect Region() const { return m_region; }
protected:
    void DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) override;
private:
    Rect m_region{};
    std::string m_path;
    std::string m_displayPath;
    SourceLoader m_loader;
    std::optional<sdk::LayeredPortrait> m_portrait;
    std::optional<sdk::LayeredPortrait> m_displayPortrait;
    std::string m_displayPortraitPath;
    ResourceRefValue m_texture;
    float m_opacity = 1.0f;
    TextureScaleMode m_scaleMode = TextureScaleMode::Stretch;
    HorizontalTextAlignment m_horizontalAlignment = HorizontalTextAlignment::Center;
    VerticalTextAlignment m_verticalAlignment = VerticalTextAlignment::Center;
    bool m_lockAspectRatio = true;
};

class MotionRect final : public Control {
public:
    using SourceLoader =
        std::function<std::optional<std::string>(std::string_view)>;
    using MarkerSink = std::function<void(const sdk::MotionAudioMarker&)>;

    explicit MotionRect(std::string name = "MotionRect") : Control(std::move(name)) {
        SetMouseFilter(MouseFilter::Ignore);
    }
    [[nodiscard]] std::string_view TypeName() const override { return "MotionRect"; }
    Status SetPath(std::string value);
    [[nodiscard]] const std::string& Path() const { return m_path; }
    // Replacing the loader reloads even an unchanged path. A failed reload
    // retains the previously playable clip and loader for preview revisions.
    Status SetSourceLoader(SourceLoader loader);
    void SetMarkerSink(MarkerSink sink) { m_markerSink = std::move(sink); }
    void SetAutoplay(bool value) {
        m_autoplay = value;
        if (!m_playingExplicit) m_playing = value;
    }
    [[nodiscard]] bool Autoplay() const { return m_autoplay; }
    void SetPlaying(bool value) { m_playingExplicit = true; m_playing = value; }
    [[nodiscard]] bool Playing() const { return m_playing; }
    Status SetSpeed(double value);
    [[nodiscard]] double Speed() const { return m_speed; }
    void Seek(double seconds);
    [[nodiscard]] double Position() const { return m_position; }
    [[nodiscard]] const sdk::MotionClip* Clip() const { return m_clip.get(); }
    void CollectImageDependencies(std::vector<std::string>& paths) const override;
    void Update(float deltaSeconds) override;

protected:
    void DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) override;

private:
    Status LoadPath(std::string value, bool forceReload);
    void EmitMarkers(double from, double to, bool includeStart);
    std::string m_path;
    SourceLoader m_loader;
    MarkerSink m_markerSink;
    std::unique_ptr<sdk::MotionClip> m_clip;
    std::unique_ptr<sdk::MotionSampler> m_sampler;
    double m_position = 0;
    double m_playbackTime = 0;
    double m_speed = 1;
    bool m_startMarkersPending = true;
    bool m_autoplay = true;
    bool m_playing = true;
    bool m_playingExplicit = false;
};

struct SpriteGlyph {
    std::string character;
    std::string path;
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
};

// One data-driven sprite alphabet replaces a control per glyph/slot.
class SpriteText final : public Control {
public:
    explicit SpriteText(std::string name = "SpriteText") : Control(std::move(name)) {
        SetMouseFilter(MouseFilter::Ignore);
    }
    [[nodiscard]] std::string_view TypeName() const override { return "SpriteText"; }
    void SetText(std::string value) { m_text = std::move(value); }
    [[nodiscard]] const std::string& Text() const { return m_text; }
    void SetGlyphs(std::vector<SpriteGlyph> value) { m_glyphs = std::move(value); }
    [[nodiscard]] const std::vector<SpriteGlyph>& Glyphs() const { return m_glyphs; }
    void SetAdvance(float value) { m_advance = value; }
    [[nodiscard]] float Advance() const { return m_advance; }
    void CollectImageDependencies(std::vector<std::string>& paths) const override;
protected:
    void DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) override;
private:
    std::string m_text;
    std::vector<SpriteGlyph> m_glyphs;
    float m_advance = 0;
};

class ProgressBar : public Control {
public:
    explicit ProgressBar(std::string name = "ProgressBar") : Control(std::move(name)) { SetMouseFilter(MouseFilter::Ignore); }
    [[nodiscard]] std::string_view TypeName() const override { return "ProgressBar"; }
    void SetRange(double minimum, double maximum);
    void SetValue(double value);
    [[nodiscard]] double Value() const { return m_value; }
    [[nodiscard]] double Minimum() const { return m_minimum; }
    [[nodiscard]] double Maximum() const { return m_maximum; }
protected:
    Vec2 MeasureOverride(Vec2 available) override;
    void DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) override;
private:
    double m_minimum = 0.0, m_maximum = 100.0, m_value = 0.0;
};

class ColorRect : public Control {
public:
    explicit ColorRect(Color color = {}, std::string name = "ColorRect") : Control(std::move(name)), m_color(color) { SetMouseFilter(MouseFilter::Ignore); }
    [[nodiscard]] std::string_view TypeName() const override { return "ColorRect"; }
    void SetColor(Color value) { m_color=value; }
    [[nodiscard]] Color GetColor() const { return m_color; }
protected: void DrawSelf(graphics::Renderer2D& renderer,const Theme& theme) override;
private: Color m_color;
};

class CheckBox : public Button {
public:
    using Toggled=std::function<void(bool)>;
    explicit CheckBox(std::string text={},std::string name="CheckBox") : Button(std::move(text),std::move(name)) {}
    [[nodiscard]] std::string_view TypeName() const override { return "CheckBox"; }
    void SetChecked(bool value){m_checked=value;SetVisualChecked(value);}
    [[nodiscard]] bool Checked() const{return m_checked;}
    void SetOnToggled(Toggled value){m_toggled=std::move(value);}
    void HandleEvent(UIEvent& event) override;
protected:void DrawSelf(graphics::Renderer2D& renderer,const Theme& theme) override;
private:bool m_checked=false;Toggled m_toggled;
};

class Slider : public Control {
public:
    using Changed=std::function<void(double)>;
    explicit Slider(std::string name="Slider") : Control(std::move(name)){SetFocusMode(FocusMode::All);}
    [[nodiscard]] std::string_view TypeName() const override{return "Slider";}
    void SetRange(double minimum,double maximum,double step=0.0);
    void SetValue(double value);
    void SetValueSilently(double value);
    [[nodiscard]] double Value() const{return m_value;}
    [[nodiscard]] double Minimum() const{return m_min;}
    [[nodiscard]] double Maximum() const{return m_max;}
    [[nodiscard]] double Step() const{return m_step;}
    void SetRailPath(std::string value){m_railPath=std::move(value);}
    void SetRailHoverPath(std::string value){m_railHoverPath=std::move(value);}
    void SetRailPressedPath(std::string value){m_railPressedPath=std::move(value);}
    void SetKnobPath(std::string value){m_knobPath=std::move(value);}
    void SetKnobHoverPath(std::string value){m_knobHoverPath=std::move(value);}
    void SetKnobPressedPath(std::string value){m_knobPressedPath=std::move(value);}
    [[nodiscard]] const std::string& RailPath() const{return m_railPath;}
    [[nodiscard]] const std::string& RailHoverPath() const{return m_railHoverPath;}
    [[nodiscard]] const std::string& RailPressedPath() const{return m_railPressedPath;}
    [[nodiscard]] const std::string& KnobPath() const{return m_knobPath;}
    [[nodiscard]] const std::string& KnobHoverPath() const{return m_knobHoverPath;}
    [[nodiscard]] const std::string& KnobPressedPath() const{return m_knobPressedPath;}
    void CollectImageDependencies(std::vector<std::string>& paths) const override;
    void SetRailHeight(float value){m_railHeight=std::max(1.0f,value);}
    void SetKnobSize(float value){m_knobSize=std::max(1.0f,value);}
    void SetFillRailToValue(bool value){m_fillRailToValue=value;}
    // Names a Player option (`player.options`) this slider edits. The host
    // sets its range and value and writes every change back.
    void SetOptionKey(std::string value){m_optionKey=std::move(value);}
    [[nodiscard]] const std::string& OptionKey() const{return m_optionKey;}
    [[nodiscard]] float RailHeight() const{return m_railHeight;}
    [[nodiscard]] float KnobSize() const{return m_knobSize;}
    [[nodiscard]] bool FillRailToValue() const{return m_fillRailToValue;}
    void SetOnChanged(Changed value){m_changed=std::move(value);}
    void HandleEvent(UIEvent& event) override;
    bool PerformAccessibilityAction(std::string_view action,
                                    std::string_view value = {}) override;
protected:Vec2 MeasureOverride(Vec2 available) override;void DrawSelf(graphics::Renderer2D& renderer,const Theme& theme) override;
private:void SetFromPosition(float x);double m_min=0,m_max=1,m_step=0,m_value=0;Changed m_changed;
    std::string m_railPath,m_railHoverPath,m_railPressedPath;
    std::string m_knobPath,m_knobHoverPath,m_knobPressedPath;
    float m_railHeight=6.0f,m_knobSize=14.0f;
    bool m_fillRailToValue=false;
    std::string m_optionKey;
};

class LineEdit : public Control {
public:
    using Submitted=std::function<void(const std::string&)>;
    explicit LineEdit(std::string text={},std::string name="LineEdit");
    [[nodiscard]] std::string_view TypeName() const override{return "LineEdit";}
    [[nodiscard]] bool CapturesTextInput() const override { return true; }
    void SetText(std::string value){m_text=std::move(value);m_cursor=m_text.size();m_selectionAnchor=m_cursor;InvalidateLayout();}
    [[nodiscard]] const std::string& Text() const{return m_text;}
    [[nodiscard]] std::size_t CaretByteOffset() const { return m_cursor; }
    [[nodiscard]] std::size_t SelectionStartByteOffset() const { return std::min(m_cursor, m_selectionAnchor); }
    [[nodiscard]] std::size_t SelectionEndByteOffset() const { return std::max(m_cursor, m_selectionAnchor); }
    void SetSelectionBytes(std::size_t start, std::size_t end);
    void SetPlaceholder(std::string value){m_placeholder=std::move(value);}
    [[nodiscard]] const std::string& Placeholder() const{return m_placeholder;}
    void SetOnSubmitted(Submitted value){m_submitted=std::move(value);}
    // Escape while focused runs this (a name prompt cancels); without a
    // handler Escape passes on.
    void SetOnCancelled(std::function<void()> value){m_cancelled=std::move(value);}
    // Text size in pixels; 0 uses the theme's.
    void SetFontSize(int value){m_fontSize=std::max(0,value);InvalidateLayout();}
    [[nodiscard]] int FontSize() const{return m_fontSize;}
    // Most characters (grapheme clusters) typing or pasting may leave; 0: no limit.
    void SetMaxLength(std::size_t value){m_maxLength=value;}
    [[nodiscard]] std::size_t MaxLength() const{return m_maxLength;}
    // Authored colours; without them the theme's are used.
    void SetTextColor(std::optional<Color> value){m_textColor=value;}
    void SetFocusColor(std::optional<Color> value){m_focusColor=value;}
    // The caret's rectangle in layout coordinates (for an IME's candidate window).
    [[nodiscard]] Rect CaretRect() const;
    void Update(float deltaSeconds) override;
    void HandleEvent(UIEvent& event) override;
    [[nodiscard]] AccessibilitySemantics DescribeAccessibility() const override;
    bool PerformAccessibilityAction(std::string_view action,
                                    std::string_view value = {}) override;
protected:Vec2 MeasureOverride(Vec2 available) override;void DrawSelf(graphics::Renderer2D& renderer,const Theme& theme) override;
private:
    void MoveCaret(std::size_t value, bool extend);
    bool DeleteSelection();
    void SetCaretFromPoint(Vec2 point, bool extend);
    // Inserts typed or pasted text at the caret, cut to MaxLength.
    void InsertText(std::string_view value);
    [[nodiscard]] int EffectiveFontSize() const{return m_fontSize>0?m_fontSize:m_themeFontSize;}
    std::string m_text,m_placeholder;
    std::size_t m_cursor=0;
    std::size_t m_selectionAnchor=0;
    bool m_pointerSelecting=false;
    Submitted m_submitted;
    std::function<void()> m_cancelled;
    int m_fontSize=0;
    int m_themeFontSize=24;
    std::size_t m_maxLength=0;
    std::optional<Color> m_textColor,m_focusColor;
    // Where the last draw put the text, relative to the field (the side
    // padding scales with its height; scrolled so the caret stays inside).
    Vec2 m_textOffset{10.0f,8.0f};
    float m_lineHeight=24.0f;
    float m_scroll=0.0f;
    Vec2 m_caret{};
    // Caret blink clock, restarted by every edit or caret move.
    float m_caretClock=0.0f;
};

class RichTextLabel : public Label {
public:
    explicit RichTextLabel(std::string markup={},std::string name="RichTextLabel");
    [[nodiscard]] std::string_view TypeName() const override{return "RichTextLabel";}
    void SetMarkup(std::string markup);
    [[nodiscard]] const std::string& Markup() const{return m_markup;}
    void SetVertical(bool value){m_vertical=value;InvalidateLayout();}
    [[nodiscard]] bool Vertical() const{return m_vertical;}
    void SetVerticalRows(std::size_t value){m_verticalRows=value;InvalidateLayout();}
    [[nodiscard]] std::size_t VerticalRows() const{return m_verticalRows;}
    [[nodiscard]] AccessibilitySemantics DescribeAccessibility() const override;
    // The whole line's markup while a typewriter shows a prefix of it, so
    // styled and ruby lines wrap the same way at every step of the reveal.
    void SetLayoutReferenceMarkup(std::string_view markup);
protected:
    Vec2 MeasureOverride(Vec2 available) override;
    void DrawSelf(graphics::Renderer2D& renderer,const Theme& theme) override;
private:
    // Styled and ruby text uses the run layout (UAX #14 with 禁則), which
    // places readings over their bases; plain text uses Label's reflow.
    [[nodiscard]] bool UsesRunLayout() const { return !m_vertical && (!m_runs.empty() || !m_ruby.empty()); }
    void DrawStyled(graphics::Renderer2D& renderer, const Theme& theme, int baseSize);
    std::string m_markup;
    struct RubyDraw{std::string prefix,base,reading;};
    std::vector<RubyDraw> m_ruby;
    // Inline [font]/[size]/[color]/[b]/[i] runs over Text(); empty when unstyled.
    std::vector<text::TextRun> m_runs;
    std::vector<text::TextRun> m_referenceRuns;
    bool m_vertical=false;
    std::size_t m_verticalRows=0;
};

class NinePatchRect : public Control {
public:
    explicit NinePatchRect(std::string path={},std::string name="NinePatchRect")
        :Control(std::move(name)),m_path(std::move(path)){SetMouseFilter(MouseFilter::Ignore);}
    [[nodiscard]] std::string_view TypeName() const override{return "NinePatchRect";}
    void SetPath(std::string value){m_path=std::move(value);}
    [[nodiscard]] const std::string& Path()const{return m_path;}
    void CollectImageDependencies(std::vector<std::string>& paths) const override;
    void SetPatchMargins(Rect value){m_margins=value;}
    [[nodiscard]] Rect PatchMargins()const{return m_margins;}
    void SetDrawCenter(bool value){m_drawCenter=value;}
    [[nodiscard]] bool DrawCenter()const{return m_drawCenter;}
    // The border is drawn this many times its size in the image (0.5 for an
    // image made at twice the layout's resolution).
    void SetPatchScale(float value){m_patchScale=value;}
    [[nodiscard]] float PatchScale()const{return m_patchScale;}
protected:void DrawSelf(graphics::Renderer2D& renderer,const Theme& theme)override;
private:std::string m_path;Rect m_margins{16,16,16,16};bool m_drawCenter=true;float m_patchScale=1.0f;
};

class TextEdit : public Control {
public:
    explicit TextEdit(std::string text={},std::string name="TextEdit");
    [[nodiscard]] std::string_view TypeName()const override{return "TextEdit";}
    [[nodiscard]] bool CapturesTextInput() const override { return true; }
    void SetText(std::string value){m_text=std::move(value);m_cursor=m_text.size();m_selectionAnchor=m_cursor;InvalidateLayout();}
    [[nodiscard]] const std::string& Text()const{return m_text;}
    [[nodiscard]] std::size_t CaretByteOffset() const { return m_cursor; }
    [[nodiscard]] std::size_t SelectionStartByteOffset() const { return std::min(m_cursor, m_selectionAnchor); }
    [[nodiscard]] std::size_t SelectionEndByteOffset() const { return std::max(m_cursor, m_selectionAnchor); }
    void SetSelectionBytes(std::size_t start, std::size_t end);
    void SetPlaceholder(std::string value){m_placeholder=std::move(value);}
    [[nodiscard]] const std::string& Placeholder()const{return m_placeholder;}
    void SetReadOnly(bool value){m_readOnly=value;}
    [[nodiscard]] bool ReadOnly()const{return m_readOnly;}
    void HandleEvent(UIEvent& event)override;
    [[nodiscard]] AccessibilitySemantics DescribeAccessibility() const override;
    bool PerformAccessibilityAction(std::string_view action,
                                    std::string_view value = {}) override;
protected:Vec2 MeasureOverride(Vec2 available)override;void DrawSelf(graphics::Renderer2D& renderer,const Theme& theme)override;
private:
    void Changed();
    void MoveCaret(std::size_t value, bool extend);
    bool DeleteSelection();
    void SetCaretFromPoint(Vec2 point, bool extend);
    std::string m_text,m_placeholder;
    std::size_t m_cursor=0;
    std::size_t m_selectionAnchor=0;
    bool m_pointerSelecting=false;
    bool m_readOnly=false;
};

class OptionButton : public Button {
public:
    explicit OptionButton(std::string name="OptionButton");
    [[nodiscard]] std::string_view TypeName()const override{return "OptionButton";}
    void SetOptions(std::vector<std::string> options);
    [[nodiscard]] const std::vector<std::string>& Options()const{return m_options;}
    void SetSelected(int value);
    [[nodiscard]] int Selected()const{return m_selected;}
    void HandleEvent(UIEvent& event)override;
protected:void DrawSelf(graphics::Renderer2D& renderer,const Theme& theme)override;
private:void SelectFromUser(int value);std::vector<std::string> m_options;int m_selected=-1;
};

class SpinBox : public Control {
public:
    explicit SpinBox(std::string name="SpinBox");
    [[nodiscard]] std::string_view TypeName()const override{return "SpinBox";}
    void SetRange(double minimum,double maximum,double step=1.0);
    void SetValue(double value);
    [[nodiscard]] double Minimum()const{return m_minimum;}
    [[nodiscard]] double Maximum()const{return m_maximum;}
    [[nodiscard]] double Step()const{return m_step;}
    [[nodiscard]] double Value()const{return m_value;}
    void HandleEvent(UIEvent& event)override;
protected:Vec2 MeasureOverride(Vec2 available)override;void DrawSelf(graphics::Renderer2D& renderer,const Theme& theme)override;
private:void ChangeFromUser(double value);double m_minimum=0,m_maximum=100,m_step=1,m_value=0;
};

class RadioButton : public CheckBox {
public:
    explicit RadioButton(std::string text={},std::string name="RadioButton"):CheckBox(std::move(text),std::move(name)){}
    [[nodiscard]] std::string_view TypeName()const override{return "RadioButton";}
    void SetGroup(std::string value){m_group=std::move(value);}
    [[nodiscard]] const std::string& Group()const{return m_group;}
    void HandleEvent(UIEvent& event)override;
protected:void DrawSelf(graphics::Renderer2D& renderer,const Theme& theme)override;
private:void ClearGroup();std::string m_group;
};

enum class SeparatorOrientation{Horizontal,Vertical};
class Separator : public Control {
public:
    explicit Separator(std::string name="Separator"):Control(std::move(name)){SetMouseFilter(MouseFilter::Ignore);}
    [[nodiscard]] std::string_view TypeName()const override{return "Separator";}
    void SetOrientation(SeparatorOrientation value){m_orientation=value;InvalidateLayout();}
    [[nodiscard]] SeparatorOrientation Orientation()const{return m_orientation;}
protected:Vec2 MeasureOverride(Vec2 available)override;void DrawSelf(graphics::Renderer2D& renderer,const Theme& theme)override;
private:SeparatorOrientation m_orientation=SeparatorOrientation::Horizontal;
};

class ScrollBar : public Slider {
public:
    explicit ScrollBar(std::string name="ScrollBar"):Slider(std::move(name)){}
    [[nodiscard]] std::string_view TypeName()const override{return "ScrollBar";}
    void SetVertical(bool value){m_vertical=value;InvalidateLayout();}
    [[nodiscard]] bool Vertical()const{return m_vertical;}
    void SetPage(double value){m_page=std::max(0.0,value);}
    [[nodiscard]] double Page()const{return m_page;}
    void HandleEvent(UIEvent& event)override;
protected:Vec2 MeasureOverride(Vec2 available)override;void DrawSelf(graphics::Renderer2D& renderer,const Theme& theme)override;
private:void SetFromPosition(Vec2 position);bool m_vertical=true;double m_page=0.1;
};

class VideoRect : public Control {
public:
    struct Playback {
        std::function<bool(std::string_view)> open;
        std::function<void()> close;
        std::function<void(float)> update;
        std::function<bool()> playing;
        std::function<graphics::TextureHandle()> texture;
        std::function<Vec2()> size;
    };
    explicit VideoRect(std::string path={},std::string name="VideoRect"):Control(std::move(name)),m_path(std::move(path)){SetMouseFilter(MouseFilter::Ignore);}
    [[nodiscard]] std::string_view TypeName()const override{return "VideoRect";}
    void SetPath(std::string value);
    [[nodiscard]] const std::string& Path()const{return m_path;}
    void SetPoster(std::string value){m_poster=std::move(value);}
    [[nodiscard]] const std::string& Poster()const{return m_poster;}
    void CollectImageDependencies(std::vector<std::string>& paths) const override;
    void SetAutoplay(bool value);
    [[nodiscard]] bool Autoplay()const{return m_autoplay;}
    void SetLoop(bool value){m_loop=value;}
    [[nodiscard]] bool Loop()const{return m_loop;}
    void SetPlaying(bool value);
    [[nodiscard]] bool Playing()const{return m_playing;}
    void SetPlayback(Playback playback);
    void Update(float deltaSeconds)override;
protected:Vec2 MeasureOverride(Vec2 available)override;void DrawSelf(graphics::Renderer2D& renderer,const Theme& theme)override;
private:std::string m_path,m_poster;bool m_autoplay=true,m_loop=false,m_playing=false;Playback m_playback;
};

}  // namespace px::ui
