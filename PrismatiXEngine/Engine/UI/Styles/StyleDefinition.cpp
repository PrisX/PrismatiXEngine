#include "Engine/UI/Styles/StyleDefinition.h"

#include <algorithm>

namespace px::ui {
namespace {

diag::Diagnostic DefinitionError(std::string code, std::string message, std::string details = {}) {
    return diag::Diagnostic{.severity = diag::Severity::Error,
                            .code = std::move(code),
                            .category = "UI.Style.Definition",
                            .message = std::move(message),
                            .details = std::move(details)};
}

template <typename T, typename Id>
T* FindById(std::vector<T>& values, const Id& id) {
    const auto found = std::find_if(values.begin(), values.end(),
                                    [&](const T& value) { return value.id == id; });
    return found == values.end() ? nullptr : &*found;
}

template <typename T, typename Id>
const T* FindById(const std::vector<T>& values, const Id& id) {
    const auto found = std::find_if(values.begin(), values.end(),
                                    [&](const T& value) { return value.id == id; });
    return found == values.end() ? nullptr : &*found;
}

template <typename T, typename Id>
Status RemoveById(std::vector<T>& values, const Id& id, std::string_view kind) {
    const auto before = values.size();
    std::erase_if(values, [&](const T& value) { return value.id == id; });
    if (before == values.size())
        return Status::Fail(DefinitionError("PXSTYLE3108", "Style definition does not exist",
                                            std::string(kind) + ": " + id.ToString()));
    return Status::Ok();
}

}  // namespace

const VariantValueDefinition* VariantAxisDefinition::FindValue(const VariantValueId& value) const {
    return FindById(values, value);
}

VariantValueDefinition* VariantAxisDefinition::FindValue(const VariantValueId& value) {
    return FindById(values, value);
}

const TokenDefinition* StyleThemeData::FindToken(const TokenId& id) const {
    return FindById(tokens, id);
}

TokenDefinition* StyleThemeData::FindToken(const TokenId& id) { return FindById(tokens, id); }

const TokenDefinition* StyleThemeData::FindTokenByName(std::string_view displayName) const {
    const auto found = std::find_if(tokens.begin(), tokens.end(), [&](const TokenDefinition& token) {
        return token.displayName == displayName;
    });
    return found == tokens.end() ? nullptr : &*found;
}

TokenDefinition* StyleThemeData::FindTokenByName(std::string_view displayName) {
    return const_cast<TokenDefinition*>(std::as_const(*this).FindTokenByName(displayName));
}

const StyleDefinition* StyleThemeData::FindStyle(const StyleId& id) const {
    return FindById(styles, id);
}

StyleDefinition* StyleThemeData::FindStyle(const StyleId& id) { return FindById(styles, id); }

const StyleDefinition* StyleThemeData::FindStyleByName(std::string_view displayName) const {
    const auto found = std::find_if(styles.begin(), styles.end(), [&](const StyleDefinition& style) {
        return style.displayName == displayName;
    });
    return found == styles.end() ? nullptr : &*found;
}

const VariantAxisDefinition* StyleThemeData::FindAxis(const VariantAxisId& id) const {
    return FindById(variantAxes, id);
}

VariantAxisDefinition* StyleThemeData::FindAxis(const VariantAxisId& id) {
    return FindById(variantAxes, id);
}

Status StyleThemeData::UpsertToken(TokenDefinition token) {
    if (token.id.Empty() || token.displayName.empty())
        return Status::Fail(DefinitionError("PXSTYLE3101", "Token requires a stable ID and name"));
    if (const auto* sameName = FindTokenByName(token.displayName);
        sameName && sameName->id != token.id)
        return Status::Fail(DefinitionError("PXSTYLE3102", "Duplicate token display name",
                                            token.displayName));
    if (auto* existing = FindToken(token.id)) *existing = std::move(token);
    else tokens.push_back(std::move(token));
    return Status::Ok();
}

Status StyleThemeData::UpsertStyle(StyleDefinition style) {
    if (style.id.Empty() || style.displayName.empty())
        return Status::Fail(DefinitionError("PXSTYLE3103", "Style requires a stable ID and name"));
    if (auto* existing = FindStyle(style.id)) *existing = std::move(style);
    else styles.push_back(std::move(style));
    return Status::Ok();
}

Status StyleThemeData::UpsertAxis(VariantAxisDefinition axis) {
    if (axis.id.Empty() || axis.displayName.empty())
        return Status::Fail(DefinitionError("PXSTYLE3104",
                                            "Variant axis requires a stable ID and name"));
    std::vector<VariantValueId> ids;
    for (const auto& value : axis.values) {
        if (value.id.Empty() || value.displayName.empty())
            return Status::Fail(DefinitionError("PXSTYLE3105",
                                                "Variant value requires a stable ID and name",
                                                axis.displayName));
        if (std::find(ids.begin(), ids.end(), value.id) != ids.end())
            return Status::Fail(DefinitionError("PXSTYLE3106", "Duplicate variant value ID",
                                                axis.displayName));
        ids.push_back(value.id);
    }
    if (!axis.defaultValue.Empty() && !axis.FindValue(axis.defaultValue))
        return Status::Fail(DefinitionError("PXSTYLE3107", "Variant default value is missing",
                                            axis.displayName));
    if (auto* existing = FindAxis(axis.id)) *existing = std::move(axis);
    else variantAxes.push_back(std::move(axis));
    return Status::Ok();
}

Status StyleThemeData::RemoveToken(const TokenId& id) {
    return RemoveById(tokens, id, "token");
}

Status StyleThemeData::RemoveStyle(const StyleId& id) {
    return RemoveById(styles, id, "style");
}

Status StyleThemeData::RemoveAxis(const VariantAxisId& id) {
    return RemoveById(variantAxes, id, "axis");
}

bool IsStyleCompatibleWith(std::span<const ControlTypeId> compatibleTypes,
                           std::string_view controlType) {
    if (compatibleTypes.empty()) return true;
    return std::any_of(compatibleTypes.begin(), compatibleTypes.end(),
                       [&](const ControlTypeId& candidate) {
                           return candidate == "*" || candidate == "Control" ||
                                  candidate == controlType;
                       });
}

}  // namespace px::ui
