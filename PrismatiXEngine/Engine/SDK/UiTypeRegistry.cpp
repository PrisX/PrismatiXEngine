#include "Engine/SDK/UiTypeRegistry.h"

#include <psa/crypto.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <limits>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_set>

namespace px::sdk {
namespace {

using Json = nlohmann::ordered_json;
constexpr std::size_t kMaximumManifestBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaximumControls = 256;
constexpr std::size_t kMaximumProperties = 512;
constexpr std::size_t kMaximumSignals = 128;
constexpr std::size_t kMaximumChoices = 256;
constexpr std::size_t kMaximumText = 4096;
constexpr std::string_view kZeroHash =
    "0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::array<std::string_view, 10> kNodeKinds{
    "button", "control", "grid", "group", "hbox",
    "image", "label", "leaf", "stack", "vbox"};

bool ValidText(const std::string& value, const bool allowEmpty = false) {
    return value.size() <= kMaximumText && (allowEmpty || !value.empty());
}

bool ValidIdentity(const std::string& value) {
    if (!ValidText(value) || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](const char byte) {
        const auto character = static_cast<unsigned char>(byte);
        return std::isalnum(character) || byte == '_' || byte == '-' ||
               byte == '.';
    });
}

bool ValidHash(const std::string& value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](const char byte) {
               return (byte >= '0' && byte <= '9') ||
                      (byte >= 'a' && byte <= 'f');
           });
}

bool ValidNodeKind(const std::string_view value) {
    return std::ranges::find(kNodeKinds, value) != kNodeKinds.end();
}

std::string Sha256(const std::string_view text) {
    static const bool cryptoReady = psa_crypto_init() == PSA_SUCCESS;
    if (!cryptoReady) return {};
    std::array<std::uint8_t, 32> digest{};
    std::size_t written = 0;
    if (psa_hash_compute(
            PSA_ALG_SHA_256,
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size(),
            digest.data(), digest.size(), &written) != PSA_SUCCESS ||
        written != digest.size())
        return {};
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest)
        output << std::setw(2) << static_cast<unsigned int>(byte);
    return output.str();
}

Json PropertyJson(const UiTypeRegistryProperty& property) {
    Json value{
        {"id", property.id},
        {"displayName", property.displayName},
        {"description", property.description},
        {"category", property.category},
        {"valueType", property.valueType},
        {"defaultValue", Json::parse(property.defaultValueJson)},
        {"writable", property.writable},
        {"bindable", property.bindable},
        {"animatable", property.animatable},
        {"advanced", property.advanced},
        {"enumChoices", property.enumChoices},
        {"resourceFilter", property.resourceFilter},
        {"editorHint", property.editorHint},
        {"multiline", property.multiline},
        {"tokenBindable", property.tokenBindable},
    };
    if (property.hasRange)
        value["range"] = {
            {"minimum", property.range.minimum},
            {"maximum", property.range.maximum},
            {"step", property.range.step},
        };
    else
        value["range"] = nullptr;
    return value;
}

Json ManifestJson(const UiTypeRegistryManifest& manifest,
                  const std::string_view contractHash) {
    Json controls = Json::array();
    for (const auto& control : manifest.controls) {
        Json properties = Json::array();
        for (const auto& property : control.properties)
            properties.push_back(PropertyJson(property));
        Json signals = Json::array();
        for (const auto& signal : control.signals) {
            Json arguments = Json::array();
            for (const auto& argument : signal.arguments)
                arguments.push_back(
                    {{"id", argument.id}, {"valueType", argument.valueType}});
            signals.push_back({
                {"id", signal.id},
                {"displayName", signal.displayName},
                {"description", signal.description},
                {"arguments", std::move(arguments)},
            });
        }
        controls.push_back({
            {"id", control.id},
            {"runtimeType", control.runtimeType},
            {"nodeKind", control.nodeKind},
            {"displayName", control.displayName},
            {"description", control.description},
            {"category", control.category},
            {"iconId", control.iconId},
            {"canHaveChildren", control.canHaveChildren},
            {"acceptedResourceKinds", control.acceptedResourceKinds},
            {"properties", std::move(properties)},
            {"signals", std::move(signals)},
        });
    }
    return {
        {"format", "PrismatiXUiTypeRegistry"},
        {"schemaRevision", kUiTypeRegistrySchemaRevision},
        {"contract", "uiTypeRegistry"},
        {"contractRevision", kUiTypeRegistryContractRevision},
        {"contractHash", contractHash},
        {"controls", std::move(controls)},
    };
}

void Diagnostic(UiTypeRegistryParseResult& result, std::string code,
                std::string message) {
    result.diagnostics.push_back({std::move(code), std::move(message)});
}

bool ExactKeys(const Json& value,
               const std::unordered_set<std::string>& expected) {
    if (!value.is_object() || value.size() != expected.size()) return false;
    for (auto entry = value.cbegin(); entry != value.cend(); ++entry) {
        if (!expected.contains(entry.key())) return false;
    }
    return true;
}

bool ReadString(const Json& object, const char* key,
                std::string& output, const bool allowEmpty = false) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) return false;
    output = found->get<std::string>();
    return ValidText(output, allowEmpty);
}

bool ReadStringArray(const Json& value,
                     std::vector<std::string>& output,
                     const std::size_t maximum) {
    if (!value.is_array() || value.size() > maximum) return false;
    std::unordered_set<std::string> unique;
    for (const auto& item : value) {
        if (!item.is_string()) return false;
        auto text = item.get<std::string>();
        if (!ValidText(text) || !unique.insert(text).second) return false;
        output.push_back(std::move(text));
    }
    return true;
}

}  // namespace

std::string SerializeUiTypeRegistry(const UiTypeRegistryManifest& manifest) {
    const auto identity = ManifestJson(manifest, kZeroHash).dump();
    const auto hash = Sha256(identity);
    if (hash.empty()) return {};
    return ManifestJson(manifest, hash).dump(2) + "\n";
}

UiTypeRegistryParseResult ParseUiTypeRegistry(const std::string_view source) {
    UiTypeRegistryParseResult result;
    if (source.empty() || source.size() > kMaximumManifestBytes) {
        Diagnostic(result, "PXSDKUITYPE1001",
                   "UI TypeRegistry manifest exceeds its byte budget");
        return result;
    }
    Json root;
    try {
        root = Json::parse(source);
    } catch (const nlohmann::json::exception&) {
        Diagnostic(result, "PXSDKUITYPE1002",
                   "UI TypeRegistry manifest is not valid JSON");
        return result;
    }
    static const std::unordered_set<std::string> rootKeys{
        "format", "schemaRevision", "contract", "contractRevision",
        "contractHash", "controls"};
    const auto schemaRevision = root.value("schemaRevision", 0u);
    const auto contractRevision = root.value("contractRevision", 0u);
    const bool supportedRevision =
        schemaRevision == kUiTypeRegistrySchemaRevision &&
        contractRevision == kUiTypeRegistryContractRevision;
    if (!ExactKeys(root, rootKeys) ||
        root.value("format", "") != "PrismatiXUiTypeRegistry" ||
        !supportedRevision ||
        root.value("contract", "") != "uiTypeRegistry" ||
        !root["contractHash"].is_string() ||
        !ValidHash(root["contractHash"].get<std::string>()) ||
        !root["controls"].is_array() ||
        root["controls"].size() > kMaximumControls) {
        Diagnostic(result, "PXSDKUITYPE1003",
                   "UI TypeRegistry manifest envelope is unsupported");
        return result;
    }
    result.manifest.schemaRevision = schemaRevision;
    result.manifest.contractRevision = contractRevision;
    result.manifest.contractHash = root["contractHash"].get<std::string>();
    auto identity = root;
    identity["contractHash"] = kZeroHash;
    if (Sha256(identity.dump()) != result.manifest.contractHash) {
        Diagnostic(result, "PXSDKUITYPE1004",
                   "UI TypeRegistry contract hash does not match its payload");
        return result;
    }
    std::unordered_set<std::string> controlIds;
    std::string previousControl;
    for (const auto& sourceControl : root["controls"]) {
        static const std::unordered_set<std::string> controlKeys{
            "id", "runtimeType", "nodeKind", "displayName", "description",
            "category", "iconId", "canHaveChildren", "acceptedResourceKinds",
            "properties", "signals"};
        UiTypeRegistryControl control;
        if (!ExactKeys(sourceControl, controlKeys) ||
            !ReadString(sourceControl, "id", control.id) ||
            !ReadString(sourceControl, "runtimeType", control.runtimeType) ||
            !ReadString(sourceControl, "nodeKind", control.nodeKind) ||
            !ReadString(sourceControl, "displayName", control.displayName) ||
            !ReadString(sourceControl, "description", control.description, true) ||
            !ReadString(sourceControl, "category", control.category) ||
            !ReadString(sourceControl, "iconId", control.iconId) ||
            !ValidIdentity(control.id) ||
            !controlIds.insert(control.id).second ||
            (!previousControl.empty() && control.id <= previousControl) ||
            !sourceControl["canHaveChildren"].is_boolean() ||
            !ReadStringArray(sourceControl["acceptedResourceKinds"],
                             control.acceptedResourceKinds, kMaximumChoices) ||
            !sourceControl["properties"].is_array() ||
            sourceControl["properties"].size() > kMaximumProperties ||
            !sourceControl["signals"].is_array() ||
            sourceControl["signals"].size() > kMaximumSignals) {
            Diagnostic(result, "PXSDKUITYPE1010",
                       "UI control descriptor is malformed or non-deterministic");
            return result;
        }
        if (!ValidNodeKind(control.nodeKind)) {
            Diagnostic(result, "PXSDKUITYPE1010",
                       "UI control nodeKind is unsupported");
            return result;
        }
        previousControl = control.id;
        control.canHaveChildren = sourceControl["canHaveChildren"].get<bool>();
        std::unordered_set<std::string> propertyIds;
        std::string previousProperty;
        for (const auto& sourceProperty : sourceControl["properties"]) {
            static const std::unordered_set<std::string> propertyKeys{
                "id", "displayName", "description", "category", "valueType",
                "defaultValue", "writable", "bindable", "animatable", "advanced",
                "enumChoices", "range", "resourceFilter", "editorHint",
                "multiline", "tokenBindable"};
            UiTypeRegistryProperty property;
            if (!ExactKeys(sourceProperty, propertyKeys) ||
                !ReadString(sourceProperty, "id", property.id) ||
                !ReadString(sourceProperty, "displayName", property.displayName) ||
                !ReadString(sourceProperty, "description", property.description,
                            true) ||
                !ReadString(sourceProperty, "category", property.category) ||
                !ReadString(sourceProperty, "valueType", property.valueType) ||
                !ReadString(sourceProperty, "resourceFilter",
                            property.resourceFilter, true) ||
                !ReadString(sourceProperty, "editorHint", property.editorHint,
                            true) ||
                !ValidIdentity(property.id) ||
                !propertyIds.insert(property.id).second ||
                (!previousProperty.empty() && property.id <= previousProperty) ||
                !sourceProperty["writable"].is_boolean() ||
                !sourceProperty["bindable"].is_boolean() ||
                !sourceProperty["animatable"].is_boolean() ||
                !sourceProperty["advanced"].is_boolean() ||
                !sourceProperty["multiline"].is_boolean() ||
                !sourceProperty["tokenBindable"].is_boolean() ||
                !ReadStringArray(sourceProperty["enumChoices"],
                                 property.enumChoices, kMaximumChoices)) {
                Diagnostic(result, "PXSDKUITYPE1011",
                           "UI property descriptor is malformed or non-deterministic");
                return result;
            }
            previousProperty = property.id;
            property.defaultValueJson = sourceProperty["defaultValue"].dump();
            property.writable = sourceProperty["writable"].get<bool>();
            property.bindable = sourceProperty["bindable"].get<bool>();
            property.animatable = sourceProperty["animatable"].get<bool>();
            property.advanced = sourceProperty["advanced"].get<bool>();
            property.multiline = sourceProperty["multiline"].get<bool>();
            property.tokenBindable = sourceProperty["tokenBindable"].get<bool>();
            if (!sourceProperty["range"].is_null()) {
                static const std::unordered_set<std::string> rangeKeys{
                    "minimum", "maximum", "step"};
                const auto& range = sourceProperty["range"];
                if (!ExactKeys(range, rangeKeys) ||
                    !range["minimum"].is_number() ||
                    !range["maximum"].is_number() ||
                    !range["step"].is_number()) {
                    Diagnostic(result, "PXSDKUITYPE1012",
                               "UI property range is malformed");
                    return result;
                }
                property.hasRange = true;
                property.range = {range["minimum"].get<double>(),
                                  range["maximum"].get<double>(),
                                  range["step"].get<double>()};
                if (!std::isfinite(property.range.minimum) ||
                    !std::isfinite(property.range.maximum) ||
                    !std::isfinite(property.range.step) ||
                    property.range.maximum < property.range.minimum ||
                    property.range.step <= 0.0) {
                    Diagnostic(result, "PXSDKUITYPE1012",
                               "UI property range is invalid");
                    return result;
                }
            }
            control.properties.push_back(std::move(property));
        }
        std::unordered_set<std::string> signalIds;
        std::string previousSignal;
        for (const auto& sourceSignal : sourceControl["signals"]) {
            static const std::unordered_set<std::string> signalKeys{
                "id", "displayName", "description", "arguments"};
            UiTypeRegistrySignal signal;
            if (!ExactKeys(sourceSignal, signalKeys) ||
                !ReadString(sourceSignal, "id", signal.id) ||
                !ReadString(sourceSignal, "displayName", signal.displayName) ||
                !ReadString(sourceSignal, "description", signal.description,
                            true) ||
                !ValidIdentity(signal.id) ||
                !signalIds.insert(signal.id).second ||
                (!previousSignal.empty() && signal.id <= previousSignal) ||
                !sourceSignal["arguments"].is_array() ||
                sourceSignal["arguments"].size() > kMaximumProperties) {
                Diagnostic(result, "PXSDKUITYPE1013",
                           "UI signal descriptor is malformed or non-deterministic");
                return result;
            }
            previousSignal = signal.id;
            for (const auto& sourceArgument : sourceSignal["arguments"]) {
                static const std::unordered_set<std::string> argumentKeys{
                    "id", "valueType"};
                UiTypeRegistrySignalArgument argument;
                if (!ExactKeys(sourceArgument, argumentKeys) ||
                    !ReadString(sourceArgument, "id", argument.id) ||
                    !ReadString(sourceArgument, "valueType",
                                argument.valueType) ||
                    !ValidIdentity(argument.id)) {
                    Diagnostic(result, "PXSDKUITYPE1014",
                               "UI signal argument is malformed");
                    return result;
                }
                signal.arguments.push_back(std::move(argument));
            }
            control.signals.push_back(std::move(signal));
        }
        result.manifest.controls.push_back(std::move(control));
    }
    return result;
}

}  // namespace px::sdk
