#pragma once

#include "Editor/Tools/UIDesigner/DocumentChangeSet.h"

#include <cstdint>

namespace px::editor {

enum class PreviewUpdate : std::uint32_t {
    None = 0,
    PatchProperties = 1u << 0,
    Relayout = 1u << 1,
    InvalidateStyle = 1u << 2,
    ReconnectBindings = 1u << 3,
    UpdateAnimations = 1u << 4,
    RebuildScene = 1u << 5,
};

constexpr PreviewUpdate operator|(PreviewUpdate lhs, PreviewUpdate rhs) {
    return static_cast<PreviewUpdate>(static_cast<std::uint32_t>(lhs) |
                                      static_cast<std::uint32_t>(rhs));
}
constexpr PreviewUpdate& operator|=(PreviewUpdate& lhs, PreviewUpdate rhs) {
    lhs = lhs | rhs;
    return lhs;
}
[[nodiscard]] constexpr bool HasPreviewUpdate(PreviewUpdate value, PreviewUpdate update) {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(update)) != 0;
}

[[nodiscard]] PreviewUpdate PlanPreviewUpdate(const DocumentChangeSet& changes,
                                              const Uuid& documentId);

}  // namespace px::editor
