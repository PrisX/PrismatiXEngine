#pragma once

#include "Engine/Core/Object.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/Variant.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace px {

enum class PropertyFlags : std::uint32_t {
    None = 0,
    Editable = 1 << 0,
    Serializable = 1 << 1,
    Bindable = 1 << 2,
    ReadOnly = 1 << 3,
    ResourcePath = 1 << 4,
};
constexpr PropertyFlags operator|(PropertyFlags a, PropertyFlags b) {
    return static_cast<PropertyFlags>(static_cast<std::uint32_t>(a) |
                                      static_cast<std::uint32_t>(b));
}
constexpr bool HasFlag(PropertyFlags value, PropertyFlags flag) {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0;
}

enum class PropertyImpact : std::uint32_t {
    None = 0,
    Paint = 1u << 0,
    Layout = 1u << 1,
    Binding = 1u << 2,
    Theme = 1u << 3,
    PreviewState = 1u << 4,
    Animation = 1u << 5,
    Structure = 1u << 6,
};
constexpr PropertyImpact operator|(PropertyImpact lhs, PropertyImpact rhs) {
    return static_cast<PropertyImpact>(static_cast<std::uint32_t>(lhs) |
                                       static_cast<std::uint32_t>(rhs));
}
constexpr bool HasImpact(PropertyImpact value, PropertyImpact impact) {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(impact)) != 0;
}

enum class PropertyOwnership { Control, ParentLayout };

struct PropertyInfo {
    std::string name;
    std::string category;
    VariantType type = VariantType::Null;
    PropertyFlags flags = PropertyFlags::Editable | PropertyFlags::Serializable |
                          PropertyFlags::Bindable;
    Variant defaultValue;
    std::function<Variant(const Object&)> get;
    std::function<Status(Object&, const Variant&)> set;
    struct EditorHint {
        std::string displayName;
        std::string description;
        std::vector<std::string> enumChoices;
        double minimum = 0.0;
        double maximum = 0.0;
        double step = 0.1;
        std::string editorId;
        std::string resourceFilter;
        bool hasRange = false;
        bool multiline = false;
        bool tokenBindable = false;
    } editor;
    PropertyImpact impact = PropertyImpact::Paint;
    bool bindable = true;
    bool animatable = false;
    bool advanced = false;
    PropertyOwnership ownership = PropertyOwnership::Control;
};

struct SignalArgumentInfo {
    std::string name;
    VariantType type = VariantType::Null;
};

struct SignalInfo {
    std::string name;
    std::string displayName;
    std::string description;
    std::vector<SignalArgumentInfo> arguments;
};

// Neutral authoring metadata shared by editor frontends.  This deliberately
// contains no ImGui or editor-renderer types so runtime and plugin type
// registration can describe palette behaviour without depending on Editor.
struct DesignerTypeMetadata {
    std::string displayName;
    std::string description;
    std::string category;
    std::string iconId;
    Vec2 defaultSize{180.0f, 52.0f};
    VariantObject defaultProperties;
    std::vector<std::string> acceptedAssetTypes;
    bool canHaveChildren = false;
    bool paletteVisible = true;
};

struct TypeInfo {
    std::string name;
    std::string base;
    std::string category;
    std::function<std::unique_ptr<Object>()> factory;
    std::vector<PropertyInfo> properties;
    std::vector<SignalInfo> signals;
    std::optional<DesignerTypeMetadata> designer;
};

class TypeRegistry {
public:
    Status Register(TypeInfo type);
    [[nodiscard]] const TypeInfo* Find(const std::string& name) const;
    [[nodiscard]] const PropertyInfo* FindProperty(const std::string& type,
                                                   const std::string& property) const;
    [[nodiscard]] const SignalInfo* FindSignal(const std::string& type,
                                               const std::string& signal) const;
    [[nodiscard]] std::vector<const SignalInfo*> SignalsForType(const std::string& type) const;
    [[nodiscard]] std::unique_ptr<Object> Create(const std::string& type) const;
    [[nodiscard]] std::vector<const TypeInfo*> TypesDerivedFrom(const std::string& base) const;
    [[nodiscard]] bool IsDerivedFrom(const std::string& type, const std::string& base) const;

    static TypeRegistry& Global();

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, TypeInfo> m_types;
};

}  // namespace px
