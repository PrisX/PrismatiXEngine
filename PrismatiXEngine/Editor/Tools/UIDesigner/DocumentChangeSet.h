#pragma once

#include "Engine/Core/Uuid.h"

#include <compare>
#include <cstdint>
#include <string>
#include <vector>

namespace px::editor {

enum class DesignerDirtyFlags : std::uint32_t {
    None = 0,
    Structure = 1u << 0,
    Layout = 1u << 1,
    Paint = 1u << 2,
    Binding = 1u << 3,
    Theme = 1u << 4,
    PreviewState = 1u << 5,
    Animation = 1u << 6,
};

constexpr DesignerDirtyFlags operator|(DesignerDirtyFlags lhs, DesignerDirtyFlags rhs) {
    return static_cast<DesignerDirtyFlags>(static_cast<std::uint32_t>(lhs) |
                                           static_cast<std::uint32_t>(rhs));
}
constexpr DesignerDirtyFlags operator&(DesignerDirtyFlags lhs, DesignerDirtyFlags rhs) {
    return static_cast<DesignerDirtyFlags>(static_cast<std::uint32_t>(lhs) &
                                           static_cast<std::uint32_t>(rhs));
}
constexpr DesignerDirtyFlags& operator|=(DesignerDirtyFlags& lhs, DesignerDirtyFlags rhs) {
    lhs = lhs | rhs;
    return lhs;
}
[[nodiscard]] constexpr bool Any(DesignerDirtyFlags flags) {
    return flags != DesignerDirtyFlags::None;
}
[[nodiscard]] constexpr bool HasDirtyFlag(DesignerDirtyFlags flags, DesignerDirtyFlags flag) {
    return Any(flags & flag);
}

inline constexpr DesignerDirtyFlags kAllDesignerContentDirty =
    DesignerDirtyFlags::Structure | DesignerDirtyFlags::Layout | DesignerDirtyFlags::Paint |
    DesignerDirtyFlags::Binding | DesignerDirtyFlags::Theme | DesignerDirtyFlags::Animation;

struct DocumentPropertyChange {
    Uuid node;
    std::string property;
    auto operator<=>(const DocumentPropertyChange&) const = default;
};

struct DocumentChangeSet {
    DesignerDirtyFlags dirty = DesignerDirtyFlags::None;
    std::vector<Uuid> nodes;
    std::vector<DocumentPropertyChange> properties;
    std::vector<Uuid> structuralRoots;
    bool historyNavigation = false;

    [[nodiscard]] bool Structural() const { return !structuralRoots.empty(); }
    [[nodiscard]] bool Empty() const {
        return !Any(dirty) && nodes.empty() && properties.empty() && structuralRoots.empty();
    }

    static DocumentChangeSet Property(Uuid node, std::string property,
                                      DesignerDirtyFlags dirty);
    static DocumentChangeSet Structure(Uuid root = {});
    static DocumentChangeSet WholeDocument(DesignerDirtyFlags dirty,
                                           bool historyNavigation = false);
    void Merge(const DocumentChangeSet& other);
};

}  // namespace px::editor
