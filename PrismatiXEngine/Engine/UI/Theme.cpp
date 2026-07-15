#include "Engine/UI/Theme.h"
#include "Engine/UI/Control.h"
#include "Engine/UI/Styles/StyleSerialization.h"

namespace px::ui {
namespace {
StyleValue Literal(Variant value) { return StyleValue::Literal(std::move(value)); }

StylePropertyMap BaseProperties() {
    return {{"background.color", Literal(Color{28, 31, 40, 255})},
            {"border.color", Literal(Color{65, 72, 91, 255})},
            {"border.width", Literal(1.0)},
            {"radius.all", Literal(6.0)},
            {"padding", Literal(Vec2{12.0f, 8.0f})},
            {"spacing", Literal(8.0)},
            {"typography.color", Literal(Color{236, 239, 244, 255})},
            {"typography.font", Literal(std::string("Content/Fonts/NotoSansTC-Bold.ttf"))},
            {"typography.size", Literal(std::int64_t{24})}};
}

StyleBlock ButtonDefaults() {
    StyleBlock block;
    block.properties["background.color"] = Literal(Color{47, 54, 72, 245});
    block.stateOverrides[StateMask(StyleState::Hover)]["background.color"] =
        Literal(Color{64, 75, 101, 255});
    block.stateOverrides[StateMask(StyleState::Pressed)]["background.color"] =
        Literal(Color{34, 40, 54, 255});
    return block;
}

void ApplyResolved(ControlStyle& style, const ResolvedStyle& resolved) {
    const auto color=[&](std::string_view id,Color& target){if(const auto* p=resolved.Find(id))if(const auto* v=p->value.TryGet<Color>())target=*v;};
    const auto number=[&](std::string_view id,float& target){if(const auto* p=resolved.Find(id)){if(const auto* real=p->value.TryGet<double>())target=static_cast<float>(*real);else if(const auto* integer=p->value.TryGet<std::int64_t>())target=static_cast<float>(*integer);}};
    color("background.color",style.normal.background);color("border.color",style.normal.border);
    color("typography.color",style.text);number("border.width",style.normal.borderWidth);
    number("radius.all",style.normal.cornerRadius);number("spacing",style.spacing);
    if(const auto* p=resolved.Find("padding"))if(const auto* v=p->value.TryGet<Vec2>())style.normal.padding=*v;
    if(const auto* p=resolved.Find("typography.font"))if(const auto* v=p->value.TryGet<std::string>())style.font=*v;
    if(const auto* p=resolved.Find("typography.size"))if(const auto* v=p->value.TryGet<std::int64_t>())style.fontSize=static_cast<int>(*v);
    style.hover=style.pressed=style.disabled=style.focused=style.normal;
}
}

Theme::Theme() {
    m_styleData.globalDefaults.properties = BaseProperties();
    m_styleData.globalDefaults.stateOverrides[StateMask(StyleState::Hover)]["background.color"] =
        Literal(Color{40, 45, 58, 255});
    m_styleData.globalDefaults.stateOverrides[StateMask(StyleState::Pressed)]["background.color"] =
        Literal(Color{22, 24, 31, 255});
    m_styleData.globalDefaults.stateOverrides[StateMask(StyleState::Disabled)]["background.color"] =
        Literal(Color{31, 33, 40, 180});
    auto& focused = m_styleData.globalDefaults.stateOverrides[StateMask(StyleState::Focused)];
    focused["background.color"] = Literal(Color{40, 45, 58, 255});
    focused["border.color"] = Literal(Color{112, 162, 255, 255});

    for (const char* type : {"Button", "IconButton", "CheckBox", "OptionButton", "RadioButton"})
        m_styleData.controlTypeDefaults.emplace(type, ButtonDefaults());

    StyleDefinition dialogue;
    dialogue.id = BuiltinDialogueStyleId();
    dialogue.displayName = "Dialogue";
    dialogue.category = "Built-in";
    dialogue.compatibleTypes = {"*"};
    dialogue.properties["background.color"] = Literal(Color{14, 17, 25, 224});
    dialogue.properties["border.color"] = Literal(Color{101, 114, 146, 190});
    dialogue.properties["radius.all"] = Literal(12.0);
    dialogue.properties["padding"] = Literal(Vec2{28.0f, 22.0f});
    dialogue.properties["typography.size"] = Literal(std::int64_t{30});
    (void)m_styleData.UpsertStyle(std::move(dialogue));
}

Result<ResolvedStyle> Theme::ResolveStyle(const StyleResolveRequest& request) const {
    const auto key=ResolvedStyleCache::MakeKey(m_styleData,request,m_styleProperties);
    if(const auto* cached=m_styleCache.Find(key))return Result<ResolvedStyle>::Success(*cached);
    auto resolved=m_styleResolver.Resolve(m_styleData,request,m_styleProperties);
    if(resolved)m_styleCache.Store(key,resolved.Value());
    return resolved;
}

ControlStyle Theme::Resolve(const Control& control) const {
    ControlStyle style;
    StyleResolveRequest request{.controlType=std::string(control.TypeName()),
                                .binding=control.StyleBinding(),
                                .activeStates=control.ActiveStyleStates()};
    auto resolved=ResolveStyle(request);if(resolved)ApplyResolved(style,resolved.Value());return style;
}

Result<Variant> Theme::ResolveToken(std::string_view name,
                                    std::optional<VariantType> expectedType) const {
    const auto* token = m_styleData.FindTokenByName(name);
    if (!token)
        return Result<Variant>::Failure(diag::Diagnostic{
            .severity = diag::Severity::Error,
            .code = "PXSTYLE3301",
            .category = "UI.Style",
            .message = "Theme token does not exist",
            .details = std::string(name)});
    return m_styleResolver.ResolveTokenValue(m_styleData, token->id, expectedType);
}

Status Theme::SetStyleData(StyleThemeData data) {
    const Status valid = m_styleResolver.ValidateTheme(data, m_styleProperties);
    if (!valid) return valid;
    m_styleData=std::move(data);m_styleCache.InvalidateTheme();
    return Status::Ok();
}

StyleId BuiltinDialogueStyleId() {
    return Uuid::FromName("prismatix.style.builtin/Dialogue");
}

Result<Theme> LoadEmbeddedTheme(const resource::TypedDocument& document){
    const auto modern=document.properties.find("styleSystem");
    if(modern==document.properties.end())return Result<Theme>::Failure(diag::Diagnostic{
        .severity=diag::Severity::Error,.code="PXUI2901",.category="UI.Theme",
        .message="Embedded UI themes require styleSystem version 3"});
    auto parsed=ParseStyleTheme(modern->second);if(!parsed)return Result<Theme>::Failure(parsed.Diagnostics());
    Theme theme;const Status installed=theme.SetStyleData(parsed.TakeValue());
    if(!installed)return Result<Theme>::Failure(installed.Diagnostics());
    return Result<Theme>::Success(std::move(theme));
}

}  // namespace px::ui
