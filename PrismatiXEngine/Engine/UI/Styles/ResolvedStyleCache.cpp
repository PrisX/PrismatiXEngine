#include "Engine/UI/Styles/ResolvedStyleCache.h"

#include <algorithm>

namespace px::ui {
namespace {
void Hash(std::size_t& seed, std::size_t value) {
    seed ^= value + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
}
}
std::size_t StyleCacheKeyHash::operator()(const StyleCacheKey& key) const noexcept {
    std::size_t seed=std::hash<std::string>{}(key.controlType);
    if(key.baseStyle) Hash(seed,UuidHash{}(*key.baseStyle));
    for(const auto& id:key.appliedStyles)Hash(seed,UuidHash{}(id));
    for(const auto& value:key.variants){Hash(seed,UuidHash{}(value.axis));Hash(seed,UuidHash{}(value.value));}
    Hash(seed,key.states);Hash(seed,key.themeRevision);Hash(seed,key.registryRevision);
    Hash(seed,key.componentRevision);Hash(seed,key.localRevision);return seed;
}
const ResolvedStyle* ResolvedStyleCache::Find(const StyleCacheKey& key) const {
    const auto found=m_entries.find(key);return found==m_entries.end()?nullptr:&found->second;
}
void ResolvedStyleCache::Store(StyleCacheKey key,ResolvedStyle value){m_entries.insert_or_assign(std::move(key),std::move(value));}
void ResolvedStyleCache::InvalidateToken(const TokenId& token){
    std::erase_if(m_entries,[&](const auto& entry){return entry.second.dependencies.tokens.contains(token);});
}
void ResolvedStyleCache::InvalidateStyle(const StyleId& style){
    std::erase_if(m_entries,[&](const auto& entry){return entry.second.dependencies.styles.contains(style);});
}
StyleCacheKey ResolvedStyleCache::MakeKey(const StyleThemeData& theme,const StyleResolveRequest& request,
                                           const StylePropertyRegistry& registry){
    StyleCacheKey key{.controlType=request.controlType,.baseStyle=request.binding.baseStyle,
        .appliedStyles=request.binding.appliedStyles,.states=request.activeStates.Mask(),
        .themeRevision=theme.revision,.registryRevision=registry.Revision(),
        .componentRevision=request.binding.componentOverrideRevision,
        .localRevision=request.binding.localOverrideRevision};
    for(const auto& [axis,value]:request.binding.variants)key.variants.push_back({axis,value});
    std::ranges::sort(key.variants,{},&VariantSelection::axis);return key;
}
}  // namespace px::ui
