#include "Engine/UI/UITypeManifest.h"

#include "Engine/Core/TypeRegistry.h"
#include "Engine/SDK/UiTypeRegistry.h"
#include "Engine/UI/UITypeRegistry.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace px::ui {
namespace {

diag::Diagnostic ManifestError(std::string code, std::string message) {
    return {.severity = diag::Severity::Error,
            .code = std::move(code),
            .category = "UI.TypeManifest",
            .message = std::move(message)};
}

std::string ValueTypeName(const VariantType type) {
    switch (type) {
        case VariantType::Null: return "null";
        case VariantType::Bool: return "boolean";
        case VariantType::Integer: return "integer";
        case VariantType::Number: return "number";
        case VariantType::String: return "string";
        case VariantType::Vec2: return "vec2";
        case VariantType::Rect: return "rect";
        case VariantType::Color: return "color";
        case VariantType::Uuid: return "uuid";
        case VariantType::ResourceRef: return "resource";
        case VariantType::TokenRef: return "token";
        case VariantType::Array: return "array";
        case VariantType::Object: return "object";
    }
    return "null";
}

std::optional<nlohmann::json> VariantJson(const Variant& value,
                                          const std::size_t depth,
                                          std::size_t& nodes) {
    constexpr std::size_t kMaximumDepth = 32;
    constexpr std::size_t kMaximumNodes = 8192;
    constexpr std::size_t kMaximumArrayEntries = 1024;
    constexpr std::size_t kMaximumObjectEntries = 256;
    if (depth > kMaximumDepth || ++nodes > kMaximumNodes) return std::nullopt;
    switch (value.Type()) {
        case VariantType::Null: return nullptr;
        case VariantType::Bool: return *value.TryGet<bool>();
        case VariantType::Integer: return *value.TryGet<std::int64_t>();
        case VariantType::Number: return *value.TryGet<double>();
        case VariantType::String: return *value.TryGet<std::string>();
        case VariantType::Vec2: {
            const auto& vector = *value.TryGet<Vec2>();
            return nlohmann::json{
                {"type", "vec2"}, {"x", vector.x}, {"y", vector.y}};
        }
        case VariantType::Rect: {
            const auto& rectangle = *value.TryGet<Rect>();
            return nlohmann::json{{"type", "rect"},
                                  {"x", rectangle.x},
                                  {"y", rectangle.y},
                                  {"width", rectangle.w},
                                  {"height", rectangle.h}};
        }
        case VariantType::Color: {
            const auto& color = *value.TryGet<Color>();
            std::ostringstream text;
            text << '#' << std::uppercase << std::hex << std::setfill('0')
                 << std::setw(2) << static_cast<unsigned int>(color.r)
                 << std::setw(2) << static_cast<unsigned int>(color.g)
                 << std::setw(2) << static_cast<unsigned int>(color.b)
                 << std::setw(2) << static_cast<unsigned int>(color.a);
            return nlohmann::json{
                {"type", "color"}, {"value", text.str()}};
        }
        case VariantType::Uuid: return value.TryGet<Uuid>()->ToString();
        case VariantType::ResourceRef: {
            const auto& reference = *value.TryGet<ResourceRefValue>();
            return reference.id.Empty() ? nlohmann::json(std::string{})
                                        : nlohmann::json(reference.id.ToString());
        }
        case VariantType::TokenRef:
            return value.TryGet<TokenRefValue>()->name;
        case VariantType::Array: {
            const auto* array = value.AsArray();
            if (!array || array->size() > kMaximumArrayEntries)
                return std::nullopt;
            auto output = nlohmann::json::array();
            for (const auto& item : *array) {
                auto encoded = VariantJson(item, depth + 1, nodes);
                if (!encoded) return std::nullopt;
                output.push_back(std::move(*encoded));
            }
            return output;
        }
        case VariantType::Object: {
            const auto* object = value.AsObject();
            if (!object || object->size() > kMaximumObjectEntries)
                return std::nullopt;
            auto output = nlohmann::json::object();
            for (const auto& [key, item] : *object) {
                if (key.empty() || key.size() > 256) return std::nullopt;
                auto encoded = VariantJson(item, depth + 1, nodes);
                if (!encoded) return std::nullopt;
                output[key] = std::move(*encoded);
            }
            return output;
        }
    }
    return std::nullopt;
}

std::optional<sdk::UiTypeRegistryControl> ControlDescriptor(
    const std::string& id, const std::string& nodeKind,
    const std::string& runtimeType) {
    sdk::UiTypeRegistryControl output;
    output.id = id;
    output.runtimeType = runtimeType;
    output.nodeKind = nodeKind;
    const auto* type = TypeRegistry::Global().Find(runtimeType);
    if (!type) return std::nullopt;
    if (type->designer) {
        output.displayName = type->designer->displayName;
        output.description = type->designer->description;
        output.category = type->designer->category;
        output.iconId = type->designer->iconId;
        output.canHaveChildren = type->designer->canHaveChildren;
        output.acceptedResourceKinds = type->designer->acceptedAssetTypes;
    } else {
        output.displayName = runtimeType;
        output.category = type->category;
        output.iconId = "ui.control";
    }
    for (const auto* property :
         TypeRegistry::Global().PropertiesForType(runtimeType)) {
        sdk::UiTypeRegistryProperty descriptor;
        descriptor.id = property->name;
        descriptor.displayName = property->editor.displayName.empty()
                                     ? property->name
                                     : property->editor.displayName;
        descriptor.description = property->editor.description;
        descriptor.category = property->category;
        descriptor.valueType = ValueTypeName(property->type);
        std::size_t defaultNodes = 0;
        auto defaultValue = VariantJson(property->defaultValue, 0, defaultNodes);
        if (!defaultValue) return std::nullopt;
        descriptor.defaultValueJson = defaultValue->dump();
        descriptor.writable =
            property->set &&
            !HasFlag(property->flags, PropertyFlags::ReadOnly);
        descriptor.bindable = property->bindable;
        descriptor.animatable = property->animatable;
        descriptor.advanced = property->advanced;
        descriptor.enumChoices = property->editor.enumChoices;
        descriptor.hasRange = property->editor.hasRange;
        descriptor.range = {property->editor.minimum, property->editor.maximum,
                            property->editor.step};
        descriptor.resourceFilter = property->editor.resourceFilter;
        descriptor.editorHint = property->editor.editorId;
        descriptor.multiline = property->editor.multiline;
        descriptor.tokenBindable = property->editor.tokenBindable;
        output.properties.push_back(std::move(descriptor));
    }
    for (const auto* signal :
         TypeRegistry::Global().SignalsForType(runtimeType)) {
        sdk::UiTypeRegistrySignal descriptor{
            .id = signal->name,
            .displayName =
                signal->displayName.empty() ? signal->name : signal->displayName,
            .description = signal->description,
        };
        for (const auto& argument : signal->arguments)
            descriptor.arguments.push_back(
                {argument.name, ValueTypeName(argument.type)});
        output.signals.push_back(std::move(descriptor));
    }
    std::ranges::sort(output.acceptedResourceKinds);
    std::ranges::sort(output.properties, {}, &sdk::UiTypeRegistryProperty::id);
    std::ranges::sort(output.signals, {}, &sdk::UiTypeRegistrySignal::id);
    return output;
}

std::string LowerCamelIdentity(const std::string& runtimeType) {
    if (runtimeType.empty()) return {};
    auto output = runtimeType;
    output.front() = static_cast<char>(
        std::tolower(static_cast<unsigned char>(output.front())));
    return output;
}

}  // namespace

Result<std::string> BuildUiTypeRegistryManifest() {
    const auto registered = RegisterBuiltinUITypes();
    if (!registered) return Result<std::string>::Failure(registered.Diagnostics());
    static const std::unordered_map<std::string, std::pair<std::string, std::string>>
        legacyMappings{
            {"Button", {"button", "button"}},
            {"Control", {"control", "control"}},
            {"GridContainer", {"grid", "grid"}},
            {"Panel", {"group", "group"}},
            {"HBoxContainer", {"hbox", "hbox"}},
            {"TextureRect", {"image", "image"}},
            {"Label", {"label", "label"}},
            {"StackContainer", {"stack", "stack"}},
            {"VBoxContainer", {"vbox", "vbox"}},
    };
    sdk::UiTypeRegistryManifest manifest;
    std::unordered_set<std::string> identities;
    for (const auto* type : TypeRegistry::Global().TypesDerivedFrom("Control")) {
        const auto legacy = legacyMappings.find(type->name);
        if (legacy == legacyMappings.end() &&
            (!type->designer || !type->designer->paletteVisible))
            continue;
        const std::string id = legacy == legacyMappings.end()
                                   ? LowerCamelIdentity(type->name)
                                   : legacy->second.first;
        const std::string nodeKind = legacy == legacyMappings.end()
                                         ? (type->designer->canHaveChildren
                                                ? "group"
                                                : "leaf")
                                         : legacy->second.second;
        if (id.empty() || !identities.insert(id).second)
            return Result<std::string>::Failure(
                ManifestError("PXUITYPE2001",
                              "Runtime UI registration has an unstable identity: " +
                                  type->name));
        auto control = ControlDescriptor(id, nodeKind, type->name);
        if (!control || control->runtimeType.empty() ||
            control->displayName.empty())
            return Result<std::string>::Failure(
                ManifestError("PXUITYPE2001",
                              "Runtime UI registration is missing or has an "
                              "unserializable default: " + type->name));
        manifest.controls.push_back(std::move(*control));
    }
    std::ranges::sort(manifest.controls, {}, &sdk::UiTypeRegistryControl::id);
    const auto serialized = sdk::SerializeUiTypeRegistry(manifest);
    if (serialized.empty())
        return Result<std::string>::Failure(
            ManifestError("PXUITYPE2002",
                          "Could not hash the UI TypeRegistry manifest"));
    return Result<std::string>::Success(serialized);
}

}  // namespace px::ui
