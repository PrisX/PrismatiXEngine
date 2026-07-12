#include "Engine/UI/Widgets.h"
#include "Engine/Text/Typography.h"

#include "Engine/Graphics/Renderer2D.h"
#include "Engine/UI/Theme.h"

#include <algorithm>
#include <cmath>
#include <SDL3/SDL_scancode.h>

namespace px::ui {
namespace {
const StyleBox& StateBox(const Control& control, const ControlStyle& style) {
    if (!control.Enabled()) return style.disabled;
    if (control.Pressed()) return style.pressed;
    if (control.Focused()) return style.focused;
    if (control.Hovered()) return style.hover;
    return style.normal;
}

void DrawBox(graphics::Renderer2D& renderer, Rect area, const StyleBox& box) {
    renderer.DrawRoundedRect(area, box.cornerRadius, box.background);
    if (box.borderWidth <= 0.0f) return;
    renderer.DrawBorder(area,box.borderWidth,box.cornerRadius,box.border);
}

graphics::HorizontalAlignment Horizontal(HorizontalTextAlignment value){switch(value){case HorizontalTextAlignment::Center:return graphics::HorizontalAlignment::Center;case HorizontalTextAlignment::Right:return graphics::HorizontalAlignment::Right;default:return graphics::HorizontalAlignment::Left;}}
graphics::VerticalAlignment Vertical(VerticalTextAlignment value){switch(value){case VerticalTextAlignment::Center:return graphics::VerticalAlignment::Center;case VerticalTextAlignment::Bottom:return graphics::VerticalAlignment::Bottom;default:return graphics::VerticalAlignment::Top;}}
graphics::ContentScaleMode ToScaleMode(TextureScaleMode value){switch(value){case TextureScaleMode::Fit:return graphics::ContentScaleMode::Fit;case TextureScaleMode::Fill:return graphics::ContentScaleMode::Fill;case TextureScaleMode::Original:return graphics::ContentScaleMode::Original;default:return graphics::ContentScaleMode::Stretch;}}
}

Vec2 Panel::MeasureOverride(Vec2 available) {
    return Control::MeasureOverride(available);
}

void Panel::ArrangeOverride(Rect finalRect) { Control::ArrangeOverride(finalRect); }

void Panel::DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) {
    DrawBox(renderer, LayoutRect(), StateBox(*this, theme.Resolve(ThemeVariant())));
}

Vec2 Label::MeasureOverride(Vec2 available) {
    const int size = m_fontSize > 0 ? m_fontSize : 24;
    const float estimatedWidth = static_cast<float>(m_text.size()) * static_cast<float>(size) * 0.55f;
    if (!m_wrap || available.x <= 0.0f) return {estimatedWidth, static_cast<float>(size) * 1.35f};
    const float lines = std::max(1.0f, std::ceil(estimatedWidth / available.x));
    return {std::min(estimatedWidth, available.x), lines * static_cast<float>(size) * 1.35f};
}

void Label::DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) {
    const auto& style = theme.Resolve(ThemeVariant());
    const int size = m_fontSize > 0 ? m_fontSize : style.fontSize;
    renderer.DrawTextInRect(m_text,LayoutRect(),style.font,size,
                            m_hasColor?m_color:(Enabled()?style.text:style.textDisabled),
                            Horizontal(m_horizontalAlignment),Vertical(m_verticalAlignment),m_wrap);
}

Button::Button(std::string text, std::string name) : Control(std::move(name)), m_text(std::move(text)) {
    SetThemeVariant("Button");
    SetFocusMode(FocusMode::All);
}

void Button::Activate() { if (Enabled() && m_activated) m_activated(); }

void Button::HandleEvent(UIEvent& event) {
    if (event.type == UIEventType::Click || event.type == UIEventType::Activate) {
        Activate(); event.handled = true;
    }
}

Vec2 Button::MeasureOverride(Vec2) {
    const float size = 24.0f;
    return {std::max(88.0f, static_cast<float>(m_text.size()) * size * 0.58f + 28.0f), 48.0f};
}

void Button::DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) {
    const auto& style = theme.Resolve(ThemeVariant());
    const auto& box = StateBox(*this, style);
    DrawBox(renderer, LayoutRect(), box);
    Rect textArea=LayoutRect();textArea.x+=12;textArea.w=std::max(0.0f,textArea.w-24);
    renderer.DrawTextInRect(m_text,textArea,style.font,style.fontSize,
                            Enabled()?style.text:style.textDisabled,
                            Horizontal(m_horizontalAlignment),Vertical(m_verticalAlignment),false);
}

Vec2 IconButton::MeasureOverride(Vec2) { return {44.0f, 44.0f}; }
void IconButton::DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) {
    const auto& style=theme.Resolve(ThemeVariant());
    Color color=Enabled()?style.text:style.textDisabled;
    if(Hovered()&&Enabled())color=style.focused.border;
    renderer.DrawTextInRect(Text(),LayoutRect(),style.font,style.fontSize,color,
                            graphics::HorizontalAlignment::Center,graphics::VerticalAlignment::Center,false);
}

void TextureRect::SetOpacity(float value) { m_opacity = std::clamp(value, 0.0f, 1.0f); }
void TextureRect::DrawSelf(graphics::Renderer2D& renderer, const Theme&) {
    if (!m_path.empty()) renderer.DrawImageInRect(m_path,LayoutRect(),ToScaleMode(m_scaleMode),
        Horizontal(m_horizontalAlignment),Vertical(m_verticalAlignment),static_cast<std::uint8_t>(m_opacity*255.0f));
}

void ProgressBar::SetRange(double minimum, double maximum) {
    m_minimum = minimum; m_maximum = std::max(minimum, maximum); SetValue(m_value);
}
void ProgressBar::SetValue(double value) { m_value = std::clamp(value, m_minimum, m_maximum); }
Vec2 ProgressBar::MeasureOverride(Vec2) { return {160.0f, 18.0f}; }
void ProgressBar::DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) {
    const auto& style = theme.Resolve(ThemeVariant());
    renderer.DrawRoundedRect(LayoutRect(), LayoutRect().h * 0.5f, style.normal.background);
    const double range = m_maximum - m_minimum;
    const float ratio = range > 0.0 ? static_cast<float>((m_value - m_minimum) / range) : 0.0f;
    Rect fill = LayoutRect(); fill.w *= ratio;
    renderer.DrawRoundedRect(fill, LayoutRect().h * 0.5f, style.focused.border);
}

void ColorRect::DrawSelf(graphics::Renderer2D& renderer,const Theme&){renderer.DrawRect(LayoutRect(),m_color);}

void CheckBox::HandleEvent(UIEvent& event){
    if(event.type==UIEventType::Click||event.type==UIEventType::Activate){m_checked=!m_checked;if(m_toggled)m_toggled(m_checked);event.handled=true;return;}
    Button::HandleEvent(event);
}
void CheckBox::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){Button::DrawSelf(renderer,theme);const Rect r=LayoutRect();const float size=std::min(22.0f,r.h-10);renderer.DrawRoundedRect({r.x+8,r.y+(r.h-size)*.5f,size,size},3,{20,23,30,255});if(m_checked)renderer.DrawRoundedRect({r.x+12,r.y+(r.h-size)*.5f+4,size-8,size-8},2,theme.Resolve(ThemeVariant()).focused.border);}

void Slider::SetRange(double minimum,double maximum,double step){m_min=minimum;m_max=std::max(minimum,maximum);m_step=std::max(0.0,step);SetValue(m_value);}
void Slider::SetValue(double value){value=std::clamp(value,m_min,m_max);if(m_step>0)value=m_min+std::round((value-m_min)/m_step)*m_step;if(value==m_value)return;m_value=value;if(m_changed)m_changed(m_value);}
void Slider::SetFromPosition(float x){const Rect r=LayoutRect();const double ratio=r.w>0?std::clamp((x-r.x)/r.w,0.0f,1.0f):0;SetValue(m_min+(m_max-m_min)*ratio);}
void Slider::HandleEvent(UIEvent& event){if(event.type==UIEventType::PointerDown||(event.type==UIEventType::PointerMove&&Pressed())){SetFromPosition(event.position.x);event.handled=true;}else if(event.type==UIEventType::KeyDown){if(event.key==SDL_SCANCODE_LEFT)SetValue(m_value-(m_step>0?m_step:(m_max-m_min)/100));else if(event.key==SDL_SCANCODE_RIGHT)SetValue(m_value+(m_step>0?m_step:(m_max-m_min)/100));else return;event.handled=true;}}
Vec2 Slider::MeasureOverride(Vec2){return{180,26};}
void Slider::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){const auto& style=theme.Resolve(ThemeVariant());const Rect r=LayoutRect();const float y=r.y+r.h*.5f-3;renderer.DrawRoundedRect({r.x,y,r.w,6},3,style.normal.background);const float ratio=m_max>m_min?static_cast<float>((m_value-m_min)/(m_max-m_min)):0;renderer.DrawRoundedRect({r.x,y,r.w*ratio,6},3,style.focused.border);renderer.DrawRoundedRect({r.x+r.w*ratio-7,r.y+r.h*.5f-7,14,14},7,style.text);}

LineEdit::LineEdit(std::string text,std::string name):Control(std::move(name)),m_text(std::move(text)),m_cursor(m_text.size()){SetFocusMode(FocusMode::All);SetThemeVariant("Default");}
void LineEdit::HandleEvent(UIEvent& event){
    if(event.type==UIEventType::TextInput){m_text.insert(m_cursor,event.text);m_cursor+=event.text.size();InvalidateLayout();event.handled=true;}
    else if(event.type==UIEventType::KeyDown){if(event.key==SDL_SCANCODE_BACKSPACE&&m_cursor>0){m_text.erase(--m_cursor,1);event.handled=true;}else if(event.key==SDL_SCANCODE_DELETE&&m_cursor<m_text.size()){m_text.erase(m_cursor,1);event.handled=true;}else if(event.key==SDL_SCANCODE_LEFT&&m_cursor>0){--m_cursor;event.handled=true;}else if(event.key==SDL_SCANCODE_RIGHT&&m_cursor<m_text.size()){++m_cursor;event.handled=true;}}
    else if(event.type==UIEventType::Activate){if(m_submitted)m_submitted(m_text);event.handled=true;}
}
Vec2 LineEdit::MeasureOverride(Vec2){return{220,44};}
void LineEdit::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){const auto& style=theme.Resolve(ThemeVariant());DrawBox(renderer,LayoutRect(),StateBox(*this,style));const std::string& shown=m_text.empty()?m_placeholder:m_text;const Color color=m_text.empty()?style.textDisabled:style.text;renderer.DrawText(shown,LayoutRect().x+10,LayoutRect().y+8,style.font,style.fontSize,color);if(Focused()){const std::string prefix=m_text.substr(0,m_cursor);const Vec2 measured=renderer.MeasureText(prefix,style.font,style.fontSize);renderer.DrawRect({LayoutRect().x+10+measured.x,LayoutRect().y+8,1,static_cast<float>(style.fontSize+4)},style.focused.border);}}

RichTextLabel::RichTextLabel(std::string markup,std::string name):Label({},std::move(name)){SetWrap(true);SetMarkup(std::move(markup));}
void RichTextLabel::SetMarkup(std::string markup){m_markup=std::move(markup);const auto parsed=text::ParseRubyMarkup(m_markup);m_ruby.clear();for(const auto& ruby:parsed.ruby)m_ruby.push_back({ruby.prefix,ruby.reading});SetText(parsed.plain);}
void RichTextLabel::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){Label::DrawSelf(renderer,theme);const auto& style=theme.Resolve(ThemeVariant());const int baseSize=FontSize()>0?FontSize():style.fontSize;for(const auto& ruby:m_ruby){const Vec2 prefix=renderer.MeasureText(ruby.prefix,style.font,baseSize);renderer.DrawText(ruby.reading,LayoutRect().x+prefix.x,LayoutRect().y-std::max(8,baseSize/2),style.font,std::max(8,baseSize/2),style.text);}}

}  // namespace px::ui
