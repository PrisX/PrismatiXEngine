#pragma once
#include "Engine/UI/Actions/ActionDescriptor.h"

#include "Engine/UI/Control.h"
#include "Engine/Graphics/Texture.h"

#include <functional>
#include <string>
#include <vector>

namespace px::ui {

enum class HorizontalTextAlignment { Left, Center, Right };
enum class VerticalTextAlignment { Top, Center, Bottom };
enum class TextureScaleMode { Stretch, Fit, Fill, Original };

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
    void SetText(std::string value) { m_text = std::move(value); InvalidateLayout(); }
    [[nodiscard]] const std::string& Text() const { return m_text; }
    void SetWrap(bool value) { m_wrap = value; InvalidateLayout(); }
    [[nodiscard]] bool Wrap() const { return m_wrap; }
    void SetFontSize(int value) { m_fontSize = value; InvalidateLayout(); }
    [[nodiscard]] int FontSize() const { return m_fontSize; }
    void SetColor(Color value) { m_color = value; m_hasColor = true; }
    void SetHorizontalAlignment(HorizontalTextAlignment value) { m_horizontalAlignment=value; }
    void SetVerticalAlignment(VerticalTextAlignment value) { m_verticalAlignment=value; }
    [[nodiscard]] HorizontalTextAlignment HorizontalAlignment() const { return m_horizontalAlignment; }
    [[nodiscard]] VerticalTextAlignment VerticalAlignment() const { return m_verticalAlignment; }

protected:
    Vec2 MeasureOverride(Vec2 available) override;
    void DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) override;

private:
    std::string m_text;
    bool m_wrap = false;
    int m_fontSize = 0;
    Color m_color{};
    bool m_hasColor = false;
    HorizontalTextAlignment m_horizontalAlignment = HorizontalTextAlignment::Left;
    VerticalTextAlignment m_verticalAlignment = VerticalTextAlignment::Top;
};

class Button : public Control {
public:
    using Activated = std::function<void()>;
    explicit Button(std::string text = {}, std::string name = "Button");
    [[nodiscard]] std::string_view TypeName() const override { return "Button"; }
    void SetText(std::string value) { m_text = std::move(value); InvalidateLayout(); }
    [[nodiscard]] const std::string& Text() const { return m_text; }
    void SetOnActivated(Activated value) { m_activated = std::move(value); }
    void SetActionInvocation(ActionInvocation value) { m_action = std::move(value); }
    [[nodiscard]] const ActionInvocation& BoundAction() const { return m_action; }
    void SetHorizontalAlignment(HorizontalTextAlignment value) { m_horizontalAlignment=value; }
    void SetVerticalAlignment(VerticalTextAlignment value) { m_verticalAlignment=value; }
    [[nodiscard]] HorizontalTextAlignment HorizontalAlignment() const { return m_horizontalAlignment; }
    [[nodiscard]] VerticalTextAlignment VerticalAlignment() const { return m_verticalAlignment; }
    void Activate();
    void HandleEvent(UIEvent& event) override;

protected:
    Vec2 MeasureOverride(Vec2 available) override;
    void DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) override;

private:
    std::string m_text;
    ActionInvocation m_action;
    Activated m_activated;
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

class TextureRect : public Control {
public:
    explicit TextureRect(std::string path = {}, std::string name = "TextureRect")
        : Control(std::move(name)), m_path(std::move(path)) { SetMouseFilter(MouseFilter::Ignore); }
    [[nodiscard]] std::string_view TypeName() const override { return "TextureRect"; }
    void SetPath(std::string value) { m_path = std::move(value); }
    [[nodiscard]] const std::string& Path() const { return m_path; }
    void SetTexture(ResourceRefValue value) {
        m_texture = std::move(value);
        m_path = m_texture.lastKnownPath;
    }
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
protected:
    void DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) override;
private:
    std::string m_path;
    ResourceRefValue m_texture;
    float m_opacity = 1.0f;
    TextureScaleMode m_scaleMode = TextureScaleMode::Stretch;
    HorizontalTextAlignment m_horizontalAlignment = HorizontalTextAlignment::Center;
    VerticalTextAlignment m_verticalAlignment = VerticalTextAlignment::Center;
    bool m_lockAspectRatio = true;
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
    [[nodiscard]] double Value() const{return m_value;}
    [[nodiscard]] double Minimum() const{return m_min;}
    [[nodiscard]] double Maximum() const{return m_max;}
    [[nodiscard]] double Step() const{return m_step;}
    void SetOnChanged(Changed value){m_changed=std::move(value);}
    void HandleEvent(UIEvent& event) override;
    bool PerformAccessibilityAction(std::string_view action,
                                    std::string_view value = {}) override;
protected:Vec2 MeasureOverride(Vec2 available) override;void DrawSelf(graphics::Renderer2D& renderer,const Theme& theme) override;
private:void SetFromPosition(float x);double m_min=0,m_max=1,m_step=0,m_value=0;Changed m_changed;
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
    void HandleEvent(UIEvent& event) override;
    [[nodiscard]] AccessibilitySemantics DescribeAccessibility() const override;
    bool PerformAccessibilityAction(std::string_view action,
                                    std::string_view value = {}) override;
protected:Vec2 MeasureOverride(Vec2 available) override;void DrawSelf(graphics::Renderer2D& renderer,const Theme& theme) override;
private:
    void MoveCaret(std::size_t value, bool extend);
    bool DeleteSelection();
    void SetCaretFromPoint(Vec2 point, bool extend);
    std::string m_text,m_placeholder;
    std::size_t m_cursor=0;
    std::size_t m_selectionAnchor=0;
    bool m_pointerSelecting=false;
    Submitted m_submitted;
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
protected:
    Vec2 MeasureOverride(Vec2 available) override;
    void DrawSelf(graphics::Renderer2D& renderer,const Theme& theme) override;
private:
    std::string m_markup;
    struct RubyDraw{std::string prefix,base,reading;};
    std::vector<RubyDraw> m_ruby;
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
    void SetPatchMargins(Rect value){m_margins=value;}
    [[nodiscard]] Rect PatchMargins()const{return m_margins;}
    void SetDrawCenter(bool value){m_drawCenter=value;}
    [[nodiscard]] bool DrawCenter()const{return m_drawCenter;}
protected:void DrawSelf(graphics::Renderer2D& renderer,const Theme& theme)override;
private:std::string m_path;Rect m_margins{16,16,16,16};bool m_drawCenter=true;
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
