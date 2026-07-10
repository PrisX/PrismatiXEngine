#pragma once

#include "Engine/UI/Control.h"

#include <functional>
#include <string>

namespace px::ui {

class Panel : public Control {
public:
    explicit Panel(std::string name = "Panel") : Control(std::move(name)) {}
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

protected:
    Vec2 MeasureOverride(Vec2 available) override;
    void DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) override;

private:
    std::string m_text;
    bool m_wrap = false;
    int m_fontSize = 0;
    Color m_color{};
    bool m_hasColor = false;
    graphics::Renderer2D* m_measureRenderer = nullptr;
};

class Button : public Control {
public:
    using Activated = std::function<void()>;
    explicit Button(std::string text = {}, std::string name = "Button");
    [[nodiscard]] std::string_view TypeName() const override { return "Button"; }
    void SetText(std::string value) { m_text = std::move(value); InvalidateLayout(); }
    [[nodiscard]] const std::string& Text() const { return m_text; }
    void SetOnActivated(Activated value) { m_activated = std::move(value); }
    void SetCommand(std::string value) { m_command = std::move(value); }
    [[nodiscard]] const std::string& Command() const { return m_command; }
    void Activate();
    void HandleEvent(UIEvent& event) override;

protected:
    Vec2 MeasureOverride(Vec2 available) override;
    void DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) override;

private:
    std::string m_text;
    std::string m_command;
    Activated m_activated;
};

class TextureRect : public Control {
public:
    explicit TextureRect(std::string path = {}, std::string name = "TextureRect")
        : Control(std::move(name)), m_path(std::move(path)) { SetMouseFilter(MouseFilter::Ignore); }
    [[nodiscard]] std::string_view TypeName() const override { return "TextureRect"; }
    void SetPath(std::string value) { m_path = std::move(value); }
    [[nodiscard]] const std::string& Path() const { return m_path; }
    void SetOpacity(float value);
    [[nodiscard]] float Opacity() const { return m_opacity; }
protected:
    void DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) override;
private:
    std::string m_path;
    float m_opacity = 1.0f;
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
    void SetChecked(bool value){m_checked=value;}
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
protected:Vec2 MeasureOverride(Vec2 available) override;void DrawSelf(graphics::Renderer2D& renderer,const Theme& theme) override;
private:void SetFromPosition(float x);double m_min=0,m_max=1,m_step=0,m_value=0;Changed m_changed;
};

class LineEdit : public Control {
public:
    using Submitted=std::function<void(const std::string&)>;
    explicit LineEdit(std::string text={},std::string name="LineEdit");
    [[nodiscard]] std::string_view TypeName() const override{return "LineEdit";}
    void SetText(std::string value){m_text=std::move(value);m_cursor=m_text.size();InvalidateLayout();}
    [[nodiscard]] const std::string& Text() const{return m_text;}
    void SetPlaceholder(std::string value){m_placeholder=std::move(value);}
    [[nodiscard]] const std::string& Placeholder() const{return m_placeholder;}
    void SetOnSubmitted(Submitted value){m_submitted=std::move(value);}
    void HandleEvent(UIEvent& event) override;
protected:Vec2 MeasureOverride(Vec2 available) override;void DrawSelf(graphics::Renderer2D& renderer,const Theme& theme) override;
private:std::string m_text,m_placeholder;std::size_t m_cursor=0;Submitted m_submitted;
};

class RichTextLabel : public Label {
public:
    explicit RichTextLabel(std::string markup={},std::string name="RichTextLabel");
    [[nodiscard]] std::string_view TypeName() const override{return "RichTextLabel";}
    void SetMarkup(std::string markup);
    [[nodiscard]] const std::string& Markup() const{return m_markup;}
private:std::string m_markup;
};

}  // namespace px::ui
