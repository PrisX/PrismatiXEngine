#pragma once

#include "Engine/UI/Styles/StyleDefinition.h"
#include "Engine/UI/Styles/StylePropertyRegistry.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace px::ui {

struct StyleSourceTraceEntry {
    StyleLayerKind layer = StyleLayerKind::ThemeGlobalDefaults;
    std::string label;
    std::size_t stackOrder = 0;
    StyleStateMask stateSelector = 0;
    std::optional<StyleId> style;
    std::optional<VariantAxisId> variantAxis;
    std::optional<VariantValueId> variantValue;

    auto operator<=>(const StyleSourceTraceEntry&) const = default;
};

struct ResolvedStyleProperty {
    Variant value;
    StyleSourceTraceEntry source;
    std::vector<StyleSourceTraceEntry> overriddenSources;
    std::vector<TokenId> tokenChain;
};

struct ResolvedStyleDependencies {
    std::unordered_set<TokenId, UuidHash> tokens;
    std::unordered_set<StyleId, UuidHash> styles;
    std::unordered_set<VariantAxisId, UuidHash> variantAxes;
};

struct ResolvedStyle {
    std::unordered_map<StylePropertyId, ResolvedStyleProperty> properties;
    ResolvedStyleDependencies dependencies;
    std::uint64_t themeRevision = 0;
    std::uint64_t propertyRegistryRevision = 0;

    [[nodiscard]] const ResolvedStyleProperty* Find(std::string_view property) const;
};

struct StyleResolveRequest {
    ControlTypeId controlType = "Control";
    ControlStyleBinding binding;
    StyleStateSet activeStates;
};

class StyleResolver {
public:
    [[nodiscard]] Result<ResolvedStyle> Resolve(const StyleThemeData& theme,
                                                const StyleResolveRequest& request,
                                                const StylePropertyRegistry& registry) const;
    [[nodiscard]] Result<Variant> ResolveTokenValue(
        const StyleThemeData& theme, const TokenId& token,
        std::optional<VariantType> expectedType = std::nullopt,
        ResolvedStyleDependencies* dependencies = nullptr,
        std::vector<TokenId>* trace = nullptr) const;
    [[nodiscard]] Status ValidateTheme(const StyleThemeData& theme,
                                       const StylePropertyRegistry& registry) const;
};

}  // namespace px::ui
