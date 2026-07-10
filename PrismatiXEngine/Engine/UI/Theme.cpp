#include "Engine/UI/Theme.h"

#include <algorithm>

namespace px::ui {

Theme::Theme() {
    ControlStyle base;
    base.font = "Content/Fonts/NotoSansTC-Bold.ttf";
    base.hover.background = {40, 45, 58, 255};
    base.pressed.background = {22, 24, 31, 255};
    base.disabled.background = {31, 33, 40, 180};
    base.focused = base.hover;
    base.focused.border = {112, 162, 255, 255};
    m_styles.emplace("Default", base);

    ControlStyle button = base;
    button.normal.background = {47, 54, 72, 245};
    button.hover.background = {64, 75, 101, 255};
    button.pressed.background = {34, 40, 54, 255};
    m_styles.emplace("Button", button);

    ControlStyle dialogue = base;
    dialogue.normal.background = {14, 17, 25, 224};
    dialogue.normal.border = {101, 114, 146, 190};
    dialogue.normal.cornerRadius = 12.0f;
    dialogue.normal.padding = {28.0f, 22.0f};
    dialogue.fontSize = 30;
    m_styles.emplace("Dialogue", dialogue);
}

void Theme::Set(std::string variant, ControlStyle style) {
    m_styles.insert_or_assign(std::move(variant), std::move(style));
    ++m_revision;
}

const ControlStyle& Theme::Resolve(std::string_view variant) const {
    if (const auto it = m_styles.find(std::string(variant)); it != m_styles.end()) {
        return it->second;
    }
    return m_styles.at("Default");
}

Result<Theme> LoadEmbeddedTheme(const resource::TypedDocument& document){
    Theme theme;const auto it=document.properties.find("theme.styles");if(it==document.properties.end())return Result<Theme>::Success(std::move(theme));
    const auto* styles=it->second.AsObject();if(!styles){diag::Diagnostic d{.severity=diag::Severity::Error,.code="PXUI2901",.category="UI.Theme",.message="theme.styles must be Object"};diag::Emit(d);return Result<Theme>::Failure(std::move(d));}
    for(const auto& [name,value]:*styles){const auto* object=value.AsObject();if(!object)continue;ControlStyle style=theme.Resolve(name);
        auto color=[&](const char* key,Color& output){if(const auto field=object->find(key);field!=object->end())if(const auto* v=field->second.TryGet<Color>())output=*v;};
        color("background",style.normal.background);color("border",style.normal.border);color("text",style.text);
        if(const auto field=object->find("font");field!=object->end())if(const auto* v=field->second.TryGet<std::string>())style.font=*v;
        if(const auto field=object->find("fontSize");field!=object->end())if(const auto* v=field->second.TryGet<std::int64_t>())style.fontSize=static_cast<int>(*v);
        if(const auto field=object->find("cornerRadius");field!=object->end())if(const auto* v=field->second.TryGet<double>())style.normal.cornerRadius=static_cast<float>(*v);
        if(const auto field=object->find("padding");field!=object->end())if(const auto* v=field->second.TryGet<Vec2>())style.normal.padding=*v;
        style.hover=style.normal;style.hover.background={static_cast<std::uint8_t>(std::min(255,style.normal.background.r+18)),static_cast<std::uint8_t>(std::min(255,style.normal.background.g+18)),static_cast<std::uint8_t>(std::min(255,style.normal.background.b+18)),style.normal.background.a};style.pressed=style.normal;style.disabled=style.normal;style.focused=style.hover;
        theme.Set(name,std::move(style));}
    return Result<Theme>::Success(std::move(theme));
}

}  // namespace px::ui
