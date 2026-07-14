#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/Variant.h"
#include "Engine/UI/Styles/StyleTypes.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::ui {

enum class StyleEditorHint : std::uint8_t {
    Default,
    Color,
    Number,
    Font,
    Enum,
    Easing,
    ShadowList,
};

struct StylePropertyDescriptor {
    StylePropertyId id;
    std::string displayName;
    std::string category;
    VariantType valueType = VariantType::Null;
    StyleEditorHint editorHint = StyleEditorHint::Default;
    bool animatable = false;
    bool inherited = false;
    std::vector<ControlTypeId> compatibleTypes;
};

class StylePropertyRegistry {
public:
    explicit StylePropertyRegistry(bool registerBuiltins = true);

    Status Register(StylePropertyDescriptor descriptor);
    Status Unregister(std::string_view id);
    [[nodiscard]] const StylePropertyDescriptor* Find(std::string_view id) const;
    [[nodiscard]] bool Supports(std::string_view property, std::string_view controlType) const;
    [[nodiscard]] std::vector<const StylePropertyDescriptor*> Descriptors() const;
    [[nodiscard]] std::uint64_t Revision() const { return m_revision; }

    Status RegisterBuiltins();

private:
    std::unordered_map<StylePropertyId, StylePropertyDescriptor> m_descriptors;
    std::uint64_t m_revision = 1;
};

[[nodiscard]] bool IsStyleValueTypeCompatible(VariantType expected, VariantType actual);
[[nodiscard]] Variant CoerceStyleValue(Variant value, VariantType expected);

}  // namespace px::ui
