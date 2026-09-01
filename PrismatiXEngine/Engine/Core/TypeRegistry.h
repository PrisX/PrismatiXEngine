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
#include <utility>
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

    PropertyInfo() = default;
    PropertyInfo(std::string propertyName, std::string propertyCategory,
                 VariantType propertyType, PropertyFlags propertyFlags,
                 Variant propertyDefaultValue,
                 std::function<Variant(const Object&)> propertyGet = {},
                 std::function<Status(Object&, const Variant&)> propertySet = {})
        : name(std::move(propertyName)),
          category(std::move(propertyCategory)),
          type(propertyType),
          flags(propertyFlags),
          defaultValue(std::move(propertyDefaultValue)),
          get(std::move(propertyGet)),
          set(std::move(propertySet)) {}
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
    // Registration ownership is Runtime metadata rather than part of the
    // authored type identity. It enables atomic extension hot reload/unload.
    std::string sourceId;
    std::function<std::unique_ptr<Object>()> factory;
    std::vector<PropertyInfo> properties;
    std::vector<SignalInfo> signals;
    std::optional<DesignerTypeMetadata> designer;

    TypeInfo() = default;
    TypeInfo(std::string typeName, std::string baseType, std::string typeCategory,
             std::function<std::unique_ptr<Object>()> typeFactory,
             std::vector<PropertyInfo> typeProperties = {},
             std::vector<SignalInfo> typeSignals = {},
             std::optional<DesignerTypeMetadata> typeDesigner = std::nullopt)
        : name(std::move(typeName)),
          base(std::move(baseType)),
          category(std::move(typeCategory)),
          factory(std::move(typeFactory)),
          properties(std::move(typeProperties)),
          signals(std::move(typeSignals)),
          designer(std::move(typeDesigner)) {}
};

class TypeRegistry {
public:
    Status Register(TypeInfo type);
    Status ReplaceSource(std::string sourceId, std::vector<TypeInfo> types);
    Status RemoveSource(std::string_view sourceId);
    [[nodiscard]] std::vector<std::string> TypesForSource(
        std::string_view sourceId) const;
    [[nodiscard]] const TypeInfo* Find(const std::string& name) const;
    [[nodiscard]] const PropertyInfo* FindProperty(const std::string& type,
                                                   const std::string& property) const;
    [[nodiscard]] std::vector<const PropertyInfo*> PropertiesForType(
        const std::string& type) const;
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
