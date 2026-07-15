#pragma once

#include "Engine/Core/Uuid.h"

#include <cstdint>
#include <string>

namespace px::ui {

// Style-system references are UUID-backed identities. Display names are metadata and may be
// changed without rewriting references in scenes or themes.
using TokenId = Uuid;
using StyleId = Uuid;
using VariantAxisId = Uuid;
using VariantValueId = Uuid;
using StylePropertyId = std::string;
using ControlTypeId = std::string;

enum class StyleState : std::uint32_t {
    Normal = 0,
    Checked = 1u << 0,
    Selected = 1u << 1,
    Focused = 1u << 2,
    Hover = 1u << 3,
    Pressed = 1u << 4,
    Disabled = 1u << 5,
};

using StyleStateMask = std::uint32_t;

[[nodiscard]] constexpr StyleStateMask StateMask(StyleState state) {
    return static_cast<StyleStateMask>(state);
}

[[nodiscard]] constexpr StyleStateMask operator|(StyleState lhs, StyleState rhs) {
    return StateMask(lhs) | StateMask(rhs);
}

class StyleStateSet {
public:
    constexpr StyleStateSet() = default;
    constexpr explicit StyleStateSet(StyleState state) : m_mask(StateMask(state)) {}
    constexpr explicit StyleStateSet(StyleStateMask mask) : m_mask(mask) {}

    constexpr void Set(StyleState state, bool active = true) {
        const StyleStateMask bit = StateMask(state);
        if (active) m_mask |= bit;
        else m_mask &= ~bit;
    }
    [[nodiscard]] constexpr bool Contains(StyleState state) const {
        const StyleStateMask bit = StateMask(state);
        return bit == 0 ? m_mask == 0 : (m_mask & bit) == bit;
    }
    [[nodiscard]] constexpr bool ContainsAll(StyleStateMask selector) const {
        return selector == 0 || (m_mask & selector) == selector;
    }
    [[nodiscard]] constexpr StyleStateMask Mask() const { return m_mask; }
    [[nodiscard]] constexpr bool Empty() const { return m_mask == 0; }

    auto operator<=>(const StyleStateSet&) const = default;

private:
    StyleStateMask m_mask = 0;
};

// Lower-priority states are applied first. Disabled is intentionally the final interaction
// state; compound selectors are ordered by their highest-priority member and specificity.
[[nodiscard]] constexpr int StyleStatePriority(StyleState state) {
    switch (state) {
        case StyleState::Normal: return 0;
        case StyleState::Checked: return 10;
        case StyleState::Selected: return 20;
        case StyleState::Focused: return 30;
        case StyleState::Hover: return 40;
        case StyleState::Pressed: return 50;
        case StyleState::Disabled: return 60;
    }
    return 0;
}

enum class StyleLayerKind : std::uint8_t {
    ThemeGlobalDefaults,
    ControlTypeDefaults,
    BaseStyle,
    VariantAxis,
    AppliedStyle,
    ActiveState,
    ComponentInstanceOverride,
    LocalControlOverride,
};

[[nodiscard]] const char* StyleLayerName(StyleLayerKind layer);
[[nodiscard]] const char* StyleStateName(StyleState state);

struct VariantSelection {
    VariantAxisId axis;
    VariantValueId value;
    auto operator<=>(const VariantSelection&) const = default;
};

}  // namespace px::ui
