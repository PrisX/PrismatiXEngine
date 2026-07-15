#include "Engine/UI/Widgets.h"
#include "Engine/Text/Typography.h"

#include "Engine/Graphics/Renderer2D.h"
#include "Engine/UI/Theme.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
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
    DrawBox(renderer, LayoutRect(), StateBox(*this, theme.Resolve(*this)));
}

Vec2 Label::MeasureOverride(Vec2 available) {
    const int size = m_fontSize > 0 ? m_fontSize : 24;
    const float estimatedWidth = static_cast<float>(m_text.size()) * static_cast<float>(size) * 0.55f;
    if (!m_wrap || available.x <= 0.0f) return {estimatedWidth, static_cast<float>(size) * 1.35f};
    const float lines = std::max(1.0f, std::ceil(estimatedWidth / available.x));
    return {std::min(estimatedWidth, available.x), lines * static_cast<float>(size) * 1.35f};
}

void Label::DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) {
    const auto& style = theme.Resolve(*this);
    const int size = m_fontSize > 0 ? m_fontSize : style.fontSize;
    renderer.DrawTextInRect(m_text,LayoutRect(),style.font,size,
                            m_hasColor?m_color:(Enabled()?style.text:style.textDisabled),
                            Horizontal(m_horizontalAlignment),Vertical(m_verticalAlignment),m_wrap);
}

Button::Button(std::string text, std::string name) : Control(std::move(name)), m_text(std::move(text)) {
    SetFocusMode(FocusMode::All);
}

void Button::Activate() {
    if (!Enabled()) return;
    EmitSignal("activated");
    if (m_activated) m_activated();
}

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
    const auto& style = theme.Resolve(*this);
    const auto& box = StateBox(*this, style);
    DrawBox(renderer, LayoutRect(), box);
    Rect textArea=LayoutRect();textArea.x+=12;textArea.w=std::max(0.0f,textArea.w-24);
    renderer.DrawTextInRect(m_text,textArea,style.font,style.fontSize,
                            Enabled()?style.text:style.textDisabled,
                            Horizontal(m_horizontalAlignment),Vertical(m_verticalAlignment),false);
}

Vec2 IconButton::MeasureOverride(Vec2) { return {44.0f, 44.0f}; }
void IconButton::DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) {
    const auto& style=theme.Resolve(*this);
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
    const auto& style = theme.Resolve(*this);
    renderer.DrawRoundedRect(LayoutRect(), LayoutRect().h * 0.5f, style.normal.background);
    const double range = m_maximum - m_minimum;
    const float ratio = range > 0.0 ? static_cast<float>((m_value - m_minimum) / range) : 0.0f;
    Rect fill = LayoutRect(); fill.w *= ratio;
    renderer.DrawRoundedRect(fill, LayoutRect().h * 0.5f, style.focused.border);
}

void ColorRect::DrawSelf(graphics::Renderer2D& renderer,const Theme&){renderer.DrawRect(LayoutRect(),m_color);}

void CheckBox::HandleEvent(UIEvent& event){
    if(event.type==UIEventType::Click||event.type==UIEventType::Activate){m_checked=!m_checked;SetVisualChecked(m_checked);EmitSignal("toggled",{{"value",m_checked}});if(m_toggled)m_toggled(m_checked);Activate();event.handled=true;return;}
    Button::HandleEvent(event);
}
void CheckBox::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){Button::DrawSelf(renderer,theme);const Rect r=LayoutRect();const float size=std::min(22.0f,r.h-10);renderer.DrawRoundedRect({r.x+8,r.y+(r.h-size)*.5f,size,size},3,{20,23,30,255});if(m_checked)renderer.DrawRoundedRect({r.x+12,r.y+(r.h-size)*.5f+4,size-8,size-8},2,theme.Resolve(*this).focused.border);}

void Slider::SetRange(double minimum,double maximum,double step){m_min=minimum;m_max=std::max(minimum,maximum);m_step=std::max(0.0,step);SetValue(m_value);}
void Slider::SetValue(double value){value=std::clamp(value,m_min,m_max);if(m_step>0)value=m_min+std::round((value-m_min)/m_step)*m_step;if(value==m_value)return;m_value=value;EmitSignal("valueChanged",{{"value",m_value}});if(m_changed)m_changed(m_value);}
void Slider::SetFromPosition(float x){const Rect r=LayoutRect();const double ratio=r.w>0?std::clamp((x-r.x)/r.w,0.0f,1.0f):0;SetValue(m_min+(m_max-m_min)*ratio);}
void Slider::HandleEvent(UIEvent& event){if(event.type==UIEventType::PointerDown||(event.type==UIEventType::PointerMove&&Pressed())){SetFromPosition(event.position.x);event.handled=true;}else if(event.type==UIEventType::KeyDown){if(event.key==SDL_SCANCODE_LEFT)SetValue(m_value-(m_step>0?m_step:(m_max-m_min)/100));else if(event.key==SDL_SCANCODE_RIGHT)SetValue(m_value+(m_step>0?m_step:(m_max-m_min)/100));else return;event.handled=true;}}
Vec2 Slider::MeasureOverride(Vec2){return{180,26};}
void Slider::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){const auto& style=theme.Resolve(*this);const Rect r=LayoutRect();const float y=r.y+r.h*.5f-3;renderer.DrawRoundedRect({r.x,y,r.w,6},3,style.normal.background);const float ratio=m_max>m_min?static_cast<float>((m_value-m_min)/(m_max-m_min)):0;renderer.DrawRoundedRect({r.x,y,r.w*ratio,6},3,style.focused.border);renderer.DrawRoundedRect({r.x+r.w*ratio-7,r.y+r.h*.5f-7,14,14},7,style.text);}

LineEdit::LineEdit(std::string text,std::string name):Control(std::move(name)),m_text(std::move(text)),m_cursor(m_text.size()){SetFocusMode(FocusMode::All);}
void LineEdit::HandleEvent(UIEvent& event){
    if(event.type==UIEventType::TextInput){m_text.insert(m_cursor,event.text);m_cursor+=event.text.size();InvalidateLayout();EmitSignal("textChanged",{{"text",m_text}});event.handled=true;}
    else if(event.type==UIEventType::KeyDown){bool changed=false;if(event.key==SDL_SCANCODE_BACKSPACE&&m_cursor>0){m_text.erase(--m_cursor,1);changed=true;event.handled=true;}else if(event.key==SDL_SCANCODE_DELETE&&m_cursor<m_text.size()){m_text.erase(m_cursor,1);changed=true;event.handled=true;}else if(event.key==SDL_SCANCODE_LEFT&&m_cursor>0){--m_cursor;event.handled=true;}else if(event.key==SDL_SCANCODE_RIGHT&&m_cursor<m_text.size()){++m_cursor;event.handled=true;}if(changed){InvalidateLayout();EmitSignal("textChanged",{{"text",m_text}});}}
    else if(event.type==UIEventType::Activate){EmitSignal("submitted",{{"text",m_text}});if(m_submitted)m_submitted(m_text);event.handled=true;}
}
Vec2 LineEdit::MeasureOverride(Vec2){return{220,44};}
void LineEdit::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){const auto& style=theme.Resolve(*this);DrawBox(renderer,LayoutRect(),StateBox(*this,style));const std::string& shown=m_text.empty()?m_placeholder:m_text;const Color color=m_text.empty()?style.textDisabled:style.text;renderer.DrawText(shown,LayoutRect().x+10,LayoutRect().y+8,style.font,style.fontSize,color);if(Focused()){const std::string prefix=m_text.substr(0,m_cursor);const Vec2 measured=renderer.MeasureText(prefix,style.font,style.fontSize);renderer.DrawRect({LayoutRect().x+10+measured.x,LayoutRect().y+8,1,static_cast<float>(style.fontSize+4)},style.focused.border);}}

RichTextLabel::RichTextLabel(std::string markup,std::string name):Label({},std::move(name)){SetWrap(true);SetMarkup(std::move(markup));}
void RichTextLabel::SetMarkup(std::string markup){m_markup=std::move(markup);const auto parsed=text::ParseRubyMarkup(m_markup);m_ruby.clear();for(const auto& ruby:parsed.ruby)m_ruby.push_back({ruby.prefix,ruby.reading});SetText(parsed.plain);}
void RichTextLabel::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){Label::DrawSelf(renderer,theme);const auto& style=theme.Resolve(*this);const int baseSize=FontSize()>0?FontSize():style.fontSize;for(const auto& ruby:m_ruby){const Vec2 prefix=renderer.MeasureText(ruby.prefix,style.font,baseSize);renderer.DrawText(ruby.reading,LayoutRect().x+prefix.x,LayoutRect().y-std::max(8,baseSize/2),style.font,std::max(8,baseSize/2),style.text);}}

void NinePatchRect::DrawSelf(graphics::Renderer2D& renderer,const Theme&){if(!m_path.empty())renderer.DrawNinePatch(m_path,LayoutRect(),m_margins,m_drawCenter);}

TextEdit::TextEdit(std::string text,std::string name):Control(std::move(name)),m_text(std::move(text)),m_cursor(m_text.size()){SetFocusMode(FocusMode::All);}
void TextEdit::Changed(){InvalidateLayout();EmitSignal("textChanged",{{"text",m_text}});}
void TextEdit::HandleEvent(UIEvent& event){if(m_readOnly)return;
    if(event.type==UIEventType::TextInput){m_text.insert(m_cursor,event.text);m_cursor+=event.text.size();Changed();event.handled=true;return;}
    if(event.type==UIEventType::Activate){m_text.insert(m_cursor,"\n");++m_cursor;Changed();event.handled=true;return;}
    if(event.type!=UIEventType::KeyDown)return;bool changed=false;
    if(event.key==SDL_SCANCODE_BACKSPACE&&m_cursor>0){m_text.erase(--m_cursor,1);changed=true;}
    else if(event.key==SDL_SCANCODE_DELETE&&m_cursor<m_text.size()){m_text.erase(m_cursor,1);changed=true;}
    else if(event.key==SDL_SCANCODE_LEFT&&m_cursor>0)--m_cursor;
    else if(event.key==SDL_SCANCODE_RIGHT&&m_cursor<m_text.size())++m_cursor;
    else if(event.key==SDL_SCANCODE_HOME)m_cursor=0;else if(event.key==SDL_SCANCODE_END)m_cursor=m_text.size();else return;
    if(changed)Changed();event.handled=true;
}
Vec2 TextEdit::MeasureOverride(Vec2 available){return{std::min(360.0f,std::max(180.0f,available.x)),std::min(180.0f,std::max(88.0f,available.y))};}
void TextEdit::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){const auto& style=theme.Resolve(*this);DrawBox(renderer,LayoutRect(),StateBox(*this,style));Rect content=LayoutRect();content.x+=10;content.y+=8;content.w=std::max(0.0f,content.w-20);content.h=std::max(0.0f,content.h-16);const bool placeholder=m_text.empty();renderer.DrawTextInRect(placeholder?m_placeholder:m_text,content,style.font,style.fontSize,placeholder?style.textDisabled:style.text,graphics::HorizontalAlignment::Left,graphics::VerticalAlignment::Top,true);}

OptionButton::OptionButton(std::string name):Button({},std::move(name)){}
void OptionButton::SetOptions(std::vector<std::string> options){m_options=std::move(options);if(m_options.empty())m_selected=-1;else m_selected=std::clamp(m_selected<0?0:m_selected,0,static_cast<int>(m_options.size()-1));SetText(m_selected>=0?m_options[static_cast<std::size_t>(m_selected)]:std::string{});}
void OptionButton::SetSelected(const int value){if(m_options.empty()){m_selected=-1;SetText({});return;}m_selected=std::clamp(value,0,static_cast<int>(m_options.size()-1));SetText(m_options[static_cast<std::size_t>(m_selected)]);}
void OptionButton::SelectFromUser(const int value){const int before=m_selected;SetSelected(value);if(m_selected!=before)EmitSignal("itemSelected",{{"index",static_cast<std::int64_t>(m_selected)},{"text",Text()}});}
void OptionButton::HandleEvent(UIEvent& event){if(event.type==UIEventType::Click||event.type==UIEventType::Activate){if(!m_options.empty())SelectFromUser((m_selected+1)%static_cast<int>(m_options.size()));Activate();event.handled=true;return;}Button::HandleEvent(event);}
void OptionButton::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){Button::DrawSelf(renderer,theme);const auto& style=theme.Resolve(*this);Rect arrow=LayoutRect();arrow.x=arrow.x+arrow.w-30;arrow.w=22;renderer.DrawTextInRect("▾",arrow,style.font,style.fontSize,style.text,graphics::HorizontalAlignment::Center,graphics::VerticalAlignment::Center,false);}

SpinBox::SpinBox(std::string name):Control(std::move(name)){SetFocusMode(FocusMode::All);}
void SpinBox::SetRange(const double minimum,const double maximum,const double step){m_minimum=minimum;m_maximum=std::max(minimum,maximum);m_step=std::max(0.000001,step);SetValue(m_value);}
void SpinBox::SetValue(double value){value=std::clamp(value,m_minimum,m_maximum);value=m_minimum+std::round((value-m_minimum)/m_step)*m_step;m_value=std::clamp(value,m_minimum,m_maximum);}
void SpinBox::ChangeFromUser(const double value){const double before=m_value;SetValue(value);if(m_value!=before)EmitSignal("valueChanged",{{"value",m_value}});}
void SpinBox::HandleEvent(UIEvent& event){if(event.type==UIEventType::Click){const Rect area=LayoutRect();ChangeFromUser(m_value+(event.position.y<area.y+area.h*.5f?m_step:-m_step));event.handled=true;}
    else if(event.type==UIEventType::KeyDown){if(event.key==SDL_SCANCODE_UP)ChangeFromUser(m_value+m_step);else if(event.key==SDL_SCANCODE_DOWN)ChangeFromUser(m_value-m_step);else return;event.handled=true;}}
Vec2 SpinBox::MeasureOverride(Vec2){return{140,44};}
void SpinBox::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){const auto& style=theme.Resolve(*this);DrawBox(renderer,LayoutRect(),StateBox(*this,style));std::ostringstream text;text<<std::setprecision(8)<<m_value;Rect valueArea=LayoutRect();valueArea.x+=10;valueArea.w=std::max(0.0f,valueArea.w-42);renderer.DrawTextInRect(text.str(),valueArea,style.font,style.fontSize,style.text,graphics::HorizontalAlignment::Left,graphics::VerticalAlignment::Center,false);Rect arrows=LayoutRect();arrows.x=arrows.x+arrows.w-30;arrows.w=26;renderer.DrawTextInRect("▲\n▼",arrows,style.font,std::max(10,style.fontSize/2),style.text,graphics::HorizontalAlignment::Center,graphics::VerticalAlignment::Center,true);}

void RadioButton::ClearGroup(){if(m_group.empty())return;scene::Node* root=this;while(root->Parent())root=root->Parent();std::vector<scene::Node*> stack{root};while(!stack.empty()){scene::Node* node=stack.back();stack.pop_back();if(auto* radio=dynamic_cast<RadioButton*>(node);radio&&radio!=this&&radio->Group()==m_group&&radio->Checked()){radio->SetChecked(false);radio->EmitSignal("toggled",{{"value",false}});}for(const auto& child:node->Children())stack.push_back(child.get());}}
void RadioButton::HandleEvent(UIEvent& event){if(event.type==UIEventType::Click||event.type==UIEventType::Activate){if(!Checked()){ClearGroup();SetChecked(true);EmitSignal("toggled",{{"value",true}});}Activate();event.handled=true;return;}CheckBox::HandleEvent(event);}
void RadioButton::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){Button::DrawSelf(renderer,theme);const Rect area=LayoutRect();const float size=std::min(22.0f,area.h-10);renderer.DrawRoundedRect({area.x+8,area.y+(area.h-size)*.5f,size,size},size*.5f,{20,23,30,255});if(Checked())renderer.DrawRoundedRect({area.x+13,area.y+(area.h-size)*.5f+5,size-10,size-10},(size-10)*.5f,theme.Resolve(*this).focused.border);}

Vec2 Separator::MeasureOverride(Vec2){return m_orientation==SeparatorOrientation::Horizontal?Vec2{80,8}:Vec2{8,80};}
void Separator::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){Rect line=LayoutRect();if(m_orientation==SeparatorOrientation::Horizontal){line.y+=line.h*.5f;line.h=1;}else{line.x+=line.w*.5f;line.w=1;}renderer.DrawRect(line,theme.Resolve(*this).normal.border);}

void ScrollBar::SetFromPosition(const Vec2 position){const Rect area=LayoutRect();const double ratio=m_vertical?(area.h>0?std::clamp((position.y-area.y)/area.h,0.0f,1.0f):0.0):(area.w>0?std::clamp((position.x-area.x)/area.w,0.0f,1.0f):0.0);SetValue(Minimum()+(Maximum()-Minimum())*ratio);}
void ScrollBar::HandleEvent(UIEvent& event){if(event.type==UIEventType::PointerDown||(event.type==UIEventType::PointerMove&&Pressed())){SetFromPosition(event.position);event.handled=true;return;}if(event.type==UIEventType::Scroll){SetValue(Value()-event.wheel*(Step()>0?Step():(Maximum()-Minimum())*.05));event.handled=true;return;}Slider::HandleEvent(event);}
Vec2 ScrollBar::MeasureOverride(Vec2){return m_vertical?Vec2{18,140}:Vec2{140,18};}
void ScrollBar::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){const auto& style=theme.Resolve(*this);const Rect area=LayoutRect();renderer.DrawRoundedRect(area,std::min(area.w,area.h)*.5f,style.normal.background);const double range=Maximum()-Minimum();const float position=range>0?static_cast<float>((Value()-Minimum())/range):0;const float page=static_cast<float>(std::clamp(m_page,0.05,1.0));Rect thumb=area;if(m_vertical){thumb.h=std::max(area.w,area.h*page);thumb.y=area.y+(area.h-thumb.h)*position;}else{thumb.w=std::max(area.h,area.w*page);thumb.x=area.x+(area.w-thumb.w)*position;}renderer.DrawRoundedRect(thumb,std::min(thumb.w,thumb.h)*.5f,style.focused.border);}

void VideoRect::SetPath(std::string value){if(m_path==value)return;const bool resume=m_playing||m_autoplay;if(m_playback.close)m_playback.close();m_playing=false;m_path=std::move(value);if(resume)SetPlaying(true);}
void VideoRect::SetAutoplay(const bool value){m_autoplay=value;if(value&&!m_playing)SetPlaying(true);}
void VideoRect::SetPlayback(Playback playback){if(m_playback.close)m_playback.close();m_playback=std::move(playback);const bool start=m_playing||m_autoplay;m_playing=false;if(start)SetPlaying(true);}
void VideoRect::SetPlaying(const bool value){if(!value){if(!m_playing)return;if(m_playback.close)m_playback.close();m_playing=false;EmitSignal("playbackStopped");return;}if(m_playing&&(!m_playback.playing||m_playback.playing()))return;const bool opened=!m_path.empty()&&m_playback.open&&m_playback.open(m_path);m_playing=opened;if(opened)EmitSignal("playbackStarted");}
void VideoRect::Update(const float deltaSeconds){Control::Update(deltaSeconds);if(!m_playing)return;if(m_playback.update)m_playback.update(deltaSeconds);if(m_playback.playing&&m_playback.playing())return;if(m_loop&&m_playback.open&&m_playback.open(m_path))return;m_playing=false;EmitSignal("playbackStopped");}
Vec2 VideoRect::MeasureOverride(Vec2){return{320,180};}
void VideoRect::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){if(m_playing&&m_playback.texture){SDL_Texture* texture=m_playback.texture();const Vec2 size=m_playback.size?m_playback.size():Vec2{};if(texture&&size.x>0&&size.y>0){const Rect area=LayoutRect();const float scale=std::min(area.w/size.x,area.h/size.y);const Rect target{area.x+(area.w-size.x*scale)*.5f,area.y+(area.h-size.y*scale)*.5f,size.x*scale,size.y*scale};renderer.DrawTexture(texture,target);return;}}if(!m_poster.empty()){renderer.DrawImageInRect(m_poster,LayoutRect(),graphics::ContentScaleMode::Fit);return;}renderer.DrawRect(LayoutRect(),{8,10,14,255});renderer.DrawTextInRect(m_path.empty()?"Video":"Video · "+m_path,LayoutRect(),theme.Resolve(*this).font,16,{150,158,174,255},graphics::HorizontalAlignment::Center,graphics::VerticalAlignment::Center,true);}

}  // namespace px::ui
