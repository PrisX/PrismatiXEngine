#pragma once

#include "Engine/UI/Styles/StyleResolver.h"

#include <unordered_map>

namespace px::ui {

struct StyleCacheKey {
    std::string controlType;
    std::optional<StyleId> baseStyle;
    std::vector<StyleId> appliedStyles;
    std::vector<VariantSelection> variants;
    StyleStateMask states = 0;
    std::uint64_t themeRevision = 0;
    std::uint64_t registryRevision = 0;
    std::uint64_t componentRevision = 0;
    std::uint64_t localRevision = 0;
    auto operator<=>(const StyleCacheKey&) const = default;
};
struct StyleCacheKeyHash { std::size_t operator()(const StyleCacheKey& key) const noexcept; };

class ResolvedStyleCache {
public:
    [[nodiscard]] const ResolvedStyle* Find(const StyleCacheKey& key) const;
    void Store(StyleCacheKey key, ResolvedStyle value);
    void InvalidateToken(const TokenId& token);
    void InvalidateStyle(const StyleId& style);
    void InvalidateTheme() { m_entries.clear(); }
    [[nodiscard]] std::size_t Size() const { return m_entries.size(); }
    [[nodiscard]] static StyleCacheKey MakeKey(const StyleThemeData& theme,
                                                const StyleResolveRequest& request,
                                                const StylePropertyRegistry& registry);
private:
    std::unordered_map<StyleCacheKey, ResolvedStyle, StyleCacheKeyHash> m_entries;
};
}  // namespace px::ui
