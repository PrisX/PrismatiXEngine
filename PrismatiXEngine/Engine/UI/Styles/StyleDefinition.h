#pragma once

#include "Engine/Core/Result.h"
#include "Engine/UI/Styles/StyleValue.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace px::ui {

using StylePropertyMap = std::unordered_map<StylePropertyId, StyleValue>;
using StateStyleMap = std::map<StyleStateMask, StylePropertyMap>;

struct StyleBlock {
    StylePropertyMap properties;
    StateStyleMap stateOverrides;
};

struct TokenDefinition {
    TokenId id;
    std::string displayName;
    VariantType type = VariantType::Null;
    StyleValue value;
    std::uint64_t revision = 1;
};

struct StyleDefinition : StyleBlock {
    StyleId id;
    std::string displayName;
    std::string category;
    std::vector<std::string> tags;
    std::vector<ControlTypeId> compatibleTypes;
    std::uint64_t revision = 1;
};

struct VariantValueDefinition : StyleBlock {
    VariantValueId id;
    std::string displayName;
    std::uint64_t revision = 1;
};

struct VariantAxisDefinition {
    VariantAxisId id;
    std::string displayName;
    VariantValueId defaultValue;
    std::vector<ControlTypeId> compatibleTypes;
    std::vector<VariantValueDefinition> values;
    std::uint64_t revision = 1;

    [[nodiscard]] const VariantValueDefinition* FindValue(const VariantValueId& value) const;
    [[nodiscard]] VariantValueDefinition* FindValue(const VariantValueId& value);
};

struct ControlStyleBinding {
    std::optional<StyleId> baseStyle;
    std::unordered_map<VariantAxisId, VariantValueId, UuidHash> variants;
    std::vector<StyleId> appliedStyles;
    StylePropertyMap componentOverrides;
    StylePropertyMap localOverrides;
    std::uint64_t componentOverrideRevision = 0;
    std::uint64_t localOverrideRevision = 0;
};

struct StyleThemeData {
    static constexpr std::int64_t CurrentStyleSystemVersion = 3;

    std::int64_t styleSystemVersion = CurrentStyleSystemVersion;
    std::uint64_t revision = 1;
    std::uint64_t globalDefaultsRevision = 1;
    StyleBlock globalDefaults;
    std::map<ControlTypeId, StyleBlock> controlTypeDefaults;
    std::vector<TokenDefinition> tokens;
    std::vector<StyleDefinition> styles;
    // Vector order is the deterministic variant-axis resolution order.
    std::vector<VariantAxisDefinition> variantAxes;

    [[nodiscard]] const TokenDefinition* FindToken(const TokenId& id) const;
    [[nodiscard]] TokenDefinition* FindToken(const TokenId& id);
    [[nodiscard]] const TokenDefinition* FindTokenByName(std::string_view displayName) const;
    [[nodiscard]] TokenDefinition* FindTokenByName(std::string_view displayName);
    [[nodiscard]] const StyleDefinition* FindStyle(const StyleId& id) const;
    [[nodiscard]] StyleDefinition* FindStyle(const StyleId& id);
    [[nodiscard]] const StyleDefinition* FindStyleByName(std::string_view displayName) const;
    [[nodiscard]] const VariantAxisDefinition* FindAxis(const VariantAxisId& id) const;
    [[nodiscard]] VariantAxisDefinition* FindAxis(const VariantAxisId& id);

    Status UpsertToken(TokenDefinition token);
    Status UpsertStyle(StyleDefinition style);
    Status UpsertAxis(VariantAxisDefinition axis);
    Status RemoveToken(const TokenId& id);
    Status RemoveStyle(const StyleId& id);
    Status RemoveAxis(const VariantAxisId& id);
};

[[nodiscard]] bool IsStyleCompatibleWith(std::span<const ControlTypeId> compatibleTypes,
                                         std::string_view controlType);

}  // namespace px::ui
