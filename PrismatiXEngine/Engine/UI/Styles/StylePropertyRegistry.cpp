#include "Engine/UI/Styles/StylePropertyRegistry.h"
#include "Engine/UI/Styles/StyleDefinition.h"

#include <algorithm>
#include <array>

namespace px::ui {
namespace {

constexpr std::array<std::string_view, 9> kRuntimeMappedStyleProperties{
    "background.color", "border.color", "border.width", "radius.all", "padding",
    "spacing", "typography.font", "typography.size", "typography.color"};

diag::Diagnostic RegistryError(std::string code, std::string message, std::string details = {}) {
    return diag::Diagnostic{.severity = diag::Severity::Error,
                            .code = std::move(code),
                            .category = "UI.Style.Registry",
                            .message = std::move(message),
                            .details = std::move(details)};
}

StylePropertyDescriptor Property(std::string id, std::string name, std::string category,
                                 VariantType type, StyleEditorHint hint, bool animatable,
                                 bool inherited = false) {
    return {.id = std::move(id),
            .displayName = std::move(name),
            .category = std::move(category),
            .valueType = type,
            .editorHint = hint,
            .animatable = animatable,
            .inherited = inherited};
}

}  // namespace

StylePropertyRegistry::StylePropertyRegistry(bool registerBuiltins) {
    if (registerBuiltins) (void)RegisterBuiltins();
}

Status StylePropertyRegistry::Register(StylePropertyDescriptor descriptor) {
    if (descriptor.id.empty() || descriptor.displayName.empty() ||
        descriptor.valueType == VariantType::Null)
        return Status::Fail(RegistryError("PXSTYLE3201",
                                          "Style property requires an ID, name, and type"));
    if (m_descriptors.contains(descriptor.id))
        return Status::Fail(RegistryError("PXSTYLE3202", "Duplicate style property",
                                          descriptor.id));
    m_descriptors.emplace(descriptor.id, std::move(descriptor));
    ++m_revision;
    return Status::Ok();
}

Status StylePropertyRegistry::Unregister(std::string_view id) {
    if (!m_descriptors.erase(std::string(id)))
        return Status::Fail(RegistryError("PXSTYLE3203", "Style property does not exist",
                                          std::string(id)));
    ++m_revision;
    return Status::Ok();
}

const StylePropertyDescriptor* StylePropertyRegistry::Find(std::string_view id) const {
    const auto found = m_descriptors.find(std::string(id));
    return found == m_descriptors.end() ? nullptr : &found->second;
}

bool StylePropertyRegistry::Supports(std::string_view property,
                                     std::string_view controlType) const {
    const auto* descriptor = Find(property);
    return descriptor && IsStyleCompatibleWith(descriptor->compatibleTypes, controlType);
}

bool StylePropertyRegistry::RuntimeSupports(std::string_view property,
                                            std::string_view controlType) const {
    const auto* descriptor = Find(property);
    return descriptor && descriptor->runtimeSupported &&
           IsStyleCompatibleWith(descriptor->compatibleTypes, controlType);
}

std::vector<const StylePropertyDescriptor*> StylePropertyRegistry::Descriptors() const {
    std::vector<const StylePropertyDescriptor*> result;
    result.reserve(m_descriptors.size());
    for (const auto& [id, descriptor] : m_descriptors) {
        (void)id;
        result.push_back(&descriptor);
    }
    std::sort(result.begin(), result.end(), [](const auto* lhs, const auto* rhs) {
        return lhs->id < rhs->id;
    });
    return result;
}

Status StylePropertyRegistry::RegisterBuiltins() {
    if (!m_descriptors.empty()) return Status::Ok();
    std::vector<StylePropertyDescriptor> builtins;
    builtins.push_back(Property("background.color", "Background", "Appearance",
                                VariantType::Color, StyleEditorHint::Color, true));
    builtins.push_back(Property("background.opacity", "Background Opacity", "Appearance",
                                VariantType::Number, StyleEditorHint::Number, true));
    builtins.push_back(Property("opacity", "Opacity", "Appearance", VariantType::Number,
                                StyleEditorHint::Number, true, true));
    builtins.push_back(Property("tint", "Tint", "Appearance", VariantType::Color,
                                StyleEditorHint::Color, true, true));
    builtins.push_back(Property("border.color", "Border Color", "Border", VariantType::Color,
                                StyleEditorHint::Color, true));
    builtins.push_back(Property("border.width", "Border Width", "Border", VariantType::Number,
                                StyleEditorHint::Number, true));
    builtins.push_back(Property("border.style", "Border Style", "Border", VariantType::String,
                                StyleEditorHint::Enum, false));
    builtins.push_back(Property("radius.all", "Radius", "Radius", VariantType::Number,
                                StyleEditorHint::Number, true));
    builtins.push_back(Property("radius.topLeft", "Top Left", "Radius", VariantType::Number,
                                StyleEditorHint::Number, true));
    builtins.push_back(Property("radius.topRight", "Top Right", "Radius", VariantType::Number,
                                StyleEditorHint::Number, true));
    builtins.push_back(Property("radius.bottomLeft", "Bottom Left", "Radius",
                                VariantType::Number, StyleEditorHint::Number, true));
    builtins.push_back(Property("radius.bottomRight", "Bottom Right", "Radius",
                                VariantType::Number, StyleEditorHint::Number, true));
    builtins.push_back(Property("padding", "Padding", "Layout", VariantType::Vec2,
                                StyleEditorHint::Default, true));
    builtins.push_back(Property("spacing", "Spacing", "Layout", VariantType::Number,
                                StyleEditorHint::Number, true));
    builtins.push_back(Property("typography.font", "Font", "Typography", VariantType::String,
                                StyleEditorHint::Font, false, true));
    builtins.push_back(Property("typography.size", "Font Size", "Typography",
                                VariantType::Integer, StyleEditorHint::Number, true, true));
    builtins.push_back(Property("typography.weight", "Font Weight", "Typography",
                                VariantType::Integer, StyleEditorHint::Number, true, true));
    builtins.push_back(Property("typography.color", "Text Color", "Typography",
                                VariantType::Color, StyleEditorHint::Color, true, true));
    builtins.push_back(Property("typography.lineHeight", "Line Height", "Typography",
                                VariantType::Number, StyleEditorHint::Number, true, true));
    builtins.push_back(Property("typography.letterSpacing", "Letter Spacing", "Typography",
                                VariantType::Number, StyleEditorHint::Number, true, true));
    builtins.push_back(Property("typography.alignment", "Text Alignment", "Typography",
                                VariantType::String, StyleEditorHint::Enum, false, true));
    builtins.push_back(Property("shadow.list", "Shadows", "Effects", VariantType::Array,
                                StyleEditorHint::ShadowList, true));
    builtins.push_back(Property("outline.color", "Outline Color", "Effects", VariantType::Color,
                                StyleEditorHint::Color, true));
    builtins.push_back(Property("outline.width", "Outline Width", "Effects",
                                VariantType::Number, StyleEditorHint::Number, true));
    builtins.push_back(Property("blur", "Blur", "Effects", VariantType::Number,
                                StyleEditorHint::Number, true));
    builtins.push_back(Property("backdropBlur", "Backdrop Blur", "Effects",
                                VariantType::Number, StyleEditorHint::Number, true));
    builtins.push_back(Property("transform.scale", "Scale", "Transform", VariantType::Vec2,
                                StyleEditorHint::Default, true));
    builtins.push_back(Property("transform.rotation", "Rotation", "Transform",
                                VariantType::Number, StyleEditorHint::Number, true));
    builtins.push_back(Property("transition.duration", "Transition Duration", "Transition",
                                VariantType::Number, StyleEditorHint::Number, true));
    builtins.push_back(Property("transition.easing", "Transition Easing", "Transition",
                                VariantType::String, StyleEditorHint::Easing, false));
    const auto runtimeSupported = RuntimeMappedStyleProperties();
    for (auto& descriptor : builtins)
        descriptor.runtimeSupported = std::find(runtimeSupported.begin(), runtimeSupported.end(),
                                                descriptor.id) != runtimeSupported.end();
    for (auto& descriptor : builtins) {
        const Status status = Register(std::move(descriptor));
        if (!status) return status;
    }
    return Status::Ok();
}

bool IsStyleValueTypeCompatible(VariantType expected, VariantType actual) {
    if (expected == actual) return true;
    return expected == VariantType::Number && actual == VariantType::Integer;
}

Variant CoerceStyleValue(Variant value, VariantType expected) {
    if (expected == VariantType::Number) {
        if (const auto* integer = value.TryGet<std::int64_t>())
            return Variant(static_cast<double>(*integer));
    }
    return value;
}

std::span<const std::string_view> RuntimeMappedStyleProperties() {
    return kRuntimeMappedStyleProperties;
}

}  // namespace px::ui
