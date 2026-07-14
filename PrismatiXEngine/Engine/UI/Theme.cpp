#include "Engine/UI/Theme.h"
#include "Engine/UI/Control.h"
#include "Engine/UI/Styles/StyleSerialization.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

namespace px::ui {
namespace {
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

Result<ResolvedStyle> Theme::ResolveStyle(const StyleResolveRequest& request) const {
    const auto key=ResolvedStyleCache::MakeKey(m_styleData,request,m_styleProperties);
    if(const auto* cached=m_styleCache.Find(key))return Result<ResolvedStyle>::Success(*cached);
    auto resolved=m_styleResolver.Resolve(m_styleData,request,m_styleProperties);
    if(resolved)m_styleCache.Store(key,resolved.Value());
    return resolved;
}

ControlStyle Theme::Resolve(const Control& control) const {
    ControlStyle style=Resolve(control.ThemeVariant());
    StyleResolveRequest request{.controlType=std::string(control.TypeName()),
                                .binding=control.StyleBinding(),
                                .activeStates=control.ActiveStyleStates()};
    auto resolved=ResolveStyle(request);if(resolved)ApplyResolved(style,resolved.Value());return style;
}

void Theme::SetToken(std::string name, Variant value) {
    const std::string tokenName=name;
    m_tokens.insert_or_assign(std::move(name), value.Clone());
    TokenDefinition token{.id=StableLegacyTokenId(tokenName),.displayName=tokenName,
                          .type=value.Type(),.value=StyleValue::Literal(std::move(value))};
    (void)m_styleData.UpsertToken(std::move(token));m_styleCache.InvalidateTheme();
    ++m_revision;
}

const Variant* Theme::FindToken(std::string_view name) const {
    const auto found = m_tokens.find(std::string(name));
    return found == m_tokens.end() ? nullptr : &found->second;
}

Status Theme::SetStyleData(StyleThemeData data) {
    std::unordered_map<TokenId,Variant,UuidHash> resolved;
    std::unordered_set<TokenId,UuidHash> visiting;
    std::function<Result<Variant>(const TokenDefinition&)> resolve=
        [&](const TokenDefinition& token)->Result<Variant>{
            if(const auto found=resolved.find(token.id);found!=resolved.end())
                return Result<Variant>::Success(found->second.Clone());
            if(!visiting.insert(token.id).second)return Result<Variant>::Failure(diag::Diagnostic{
                .severity=diag::Severity::Error,.code="PXSTYLE3251",
                .category="UI.Style.Serialization",.message="Style token cycle detected",
                .details=token.displayName});
            Result<Variant> value=Result<Variant>::Success(Variant{});
            if(token.value.IsLiteral())value=Result<Variant>::Success(token.value.LiteralValue().Clone());
            else if(token.value.IsTokenReference()){
                const auto* dependency=data.FindToken(token.value.TokenReference());
                value=dependency?resolve(*dependency):Result<Variant>::Failure(diag::Diagnostic{
                    .severity=diag::Severity::Error,.code="PXSTYLE3252",
                    .category="UI.Style.Serialization",.message="Style token reference is missing",
                    .details=token.value.LastKnownTokenName()});
            }
            visiting.erase(token.id);if(value)resolved.emplace(token.id,value.Value().Clone());return value;
        };
    std::unordered_map<std::string,Variant> named;
    for(const auto& token:data.tokens){auto value=resolve(token);if(!value)return Status::Fail(value.Diagnostics());named.emplace(token.displayName,value.TakeValue());}
    m_tokens=std::move(named);m_styleData=std::move(data);m_styleCache.InvalidateTheme();++m_revision;
    return Status::Ok();
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
