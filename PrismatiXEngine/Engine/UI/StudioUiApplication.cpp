#include "Engine/UI/StudioUiApplication.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Engine/Core/TypeRegistry.h"
#include "Engine/Core/Uuid.h"
#include "Engine/UI/UIContext.h"
#include "Engine/UI/UITypeRegistry.h"

namespace px::ui {
namespace {

using Json = nlohmann::json;

diag::Diagnostic ApplicationError(std::string code, std::string message, const std::string& sourcePath, std::string nodeId = {}, std::string property = {}) {
    diag::Diagnostic diagnostic{ .severity = diag::Severity::Error, .code = std::move(code), .category = "UI.StudioApplication", .message = std::move(message) };
    diagnostic.source.path = sourcePath;
    diagnostic.source.nodeId = std::move(nodeId);
    diagnostic.source.property = std::move(property);
    return diagnostic;
}

Color ParseColor(const std::string_view value) {
    const auto byte = [value](const std::size_t offset) {
        unsigned parsed = 0;
        const auto* first = value.data() + offset;
        (void)std::from_chars(first, first + 2, parsed, 16);
        return static_cast<std::uint8_t>(parsed);
    };
    return { byte(1), byte(3), byte(5), value.size() == 9 ? byte(7) : std::uint8_t{ 255 } };
}

std::optional<Variant> RuntimeActionValue(const sdk::StudioUiActionValue& value) {
    if (std::holds_alternative<std::monostate>(value)) return Variant{};
    if (const auto* item = std::get_if<bool>(&value)) return Variant(*item);
    if (const auto* item = std::get_if<std::int64_t>(&value)) return Variant(*item);
    if (const auto* item = std::get_if<double>(&value)) return std::isfinite(*item) ? std::optional<Variant>{ Variant(*item) } : std::nullopt;
    if (const auto* item = std::get_if<std::string>(&value)) return Variant(*item);
    if (const auto* item = std::get_if<sdk::StudioUiVec2Value>(&value)) return Variant(Vec2{ item->x, item->y });
    if (const auto* item = std::get_if<sdk::StudioUiRectValue>(&value)) return Variant(Rect{ item->x, item->y, item->width, item->height });
    if (const auto* item = std::get_if<sdk::StudioUiColorValue>(&value)) return Variant(ParseColor(item->value));
    if (const auto* item = std::get_if<sdk::StudioUiNodeReferenceValue>(&value)) {
        const auto id = Uuid::Parse(item->nodeId);
        return id ? std::optional<Variant>{ Variant(*id) } : std::nullopt;
    }
    return std::nullopt;
}

std::vector<diag::Diagnostic> ParseDiagnostics(const sdk::StudioUiParseResult& parsed, const std::string& sourcePath) {
    std::vector<diag::Diagnostic> diagnostics;
    diagnostics.reserve(parsed.diagnostics.size());
    for (const auto& source : parsed.diagnostics) {
        auto diagnostic = ApplicationError(source.code, source.message, sourcePath);
        diagnostic.details = "nodeIndex=" + std::to_string(source.nodeIndex);
        diagnostics.push_back(std::move(diagnostic));
    }
    return diagnostics;
}

std::vector<diag::Diagnostic> ComponentParseDiagnostics(const sdk::StudioUiComponentParseResult& parsed, const std::string& sourcePath) {
    std::vector<diag::Diagnostic> diagnostics;
    diagnostics.reserve(parsed.diagnostics.size());
    for (const auto& source : parsed.diagnostics) {
        auto diagnostic = ApplicationError(source.code, source.message, sourcePath);
        diagnostic.details = "nodeIndex=" + std::to_string(source.nodeIndex);
        diagnostics.push_back(std::move(diagnostic));
    }
    return diagnostics;
}

bool IsBoundedRuntimeJson(const Json& value, const std::size_t depth, std::size_t& nodes) {
    constexpr std::size_t kMaxDepth = 32;
    constexpr std::size_t kMaxNodes = 8192;
    constexpr std::size_t kMaxArrayEntries = 1024;
    constexpr std::size_t kMaxObjectEntries = 256;
    if (depth > kMaxDepth || ++nodes > kMaxNodes) return false;
    if (value.is_null() || value.is_boolean() || value.is_string() || value.is_number_integer()) return true;
    if (value.is_number_float()) return std::isfinite(value.get<double>());
    if (value.is_array()) {
        if (value.size() > kMaxArrayEntries) return false;
        return std::ranges::all_of(value, [&](const Json& item) { return IsBoundedRuntimeJson(item, depth + 1, nodes); });
    }
    if (value.is_object()) {
        if (value.size() > kMaxObjectEntries) return false;
        return std::ranges::all_of(value.items(), [&](const auto& item) { return !item.key().empty() && item.key().size() <= 256 && IsBoundedRuntimeJson(item.value(), depth + 1, nodes); });
    }
    return false;
}

bool IsBoundedRuntimeJson(const Json& value) {
    std::size_t nodes = 0;
    return IsBoundedRuntimeJson(value, 0, nodes);
}

std::optional<sdk::StudioUiValue> ComponentValue(const sdk::StudioUiComponentValueType type, const sdk::StudioUiComponentPublicValue& source) {
    constexpr std::size_t kMaxJsonBytes = 1024 * 1024;
    if (source.json.size() > kMaxJsonBytes) return std::nullopt;
    const Json value = Json::parse(source.json, nullptr, false);
    if (value.is_discarded()) return std::nullopt;
    switch (type) {
        case sdk::StudioUiComponentValueType::Null:
            return value.is_null() ? std::optional<sdk::StudioUiValue>{ std::monostate{} } : std::nullopt;
        case sdk::StudioUiComponentValueType::Boolean:
            return value.is_boolean() ? std::optional<sdk::StudioUiValue>{ value.get<bool>() } : std::nullopt;
        case sdk::StudioUiComponentValueType::Integer:
            return value.is_number_integer() ? std::optional<sdk::StudioUiValue>{ value.get<std::int64_t>() } : std::nullopt;
        case sdk::StudioUiComponentValueType::Number:
            return value.is_number() ? std::optional<sdk::StudioUiValue>{ value.get<double>() } : std::nullopt;
        case sdk::StudioUiComponentValueType::String:
            return value.is_string() ? std::optional<sdk::StudioUiValue>{ value.get<std::string>() } : std::nullopt;
        case sdk::StudioUiComponentValueType::Vec2:
            if (value.is_object() && value.value("type", std::string{}) == "vec2" && value.contains("x") && value["x"].is_number() && value.contains("y") && value["y"].is_number())
                return sdk::StudioUiVec2Value{ value["x"].get<float>(), value["y"].get<float>() };
            return std::nullopt;
        case sdk::StudioUiComponentValueType::Rect:
            if (value.is_object() && value.value("type", std::string{}) == "rect" && value.contains("x") && value["x"].is_number() && value.contains("y") && value["y"].is_number() && value.contains("width") && value["width"].is_number() &&
                value.contains("height") && value["height"].is_number())
                return sdk::StudioUiRectValue{ value["x"].get<float>(), value["y"].get<float>(), value["width"].get<float>(), value["height"].get<float>() };
            return std::nullopt;
        case sdk::StudioUiComponentValueType::Color:
            if (value.is_object() && value.value("type", std::string{}) == "color" && value.contains("value") && value["value"].is_string()) return sdk::StudioUiColorValue{ value["value"].get<std::string>() };
            return std::nullopt;
        case sdk::StudioUiComponentValueType::Uuid:
            if (value.is_string()) {
                auto text = value.get<std::string>();
                if (Uuid::Parse(text)) return sdk::StudioUiUuidValue{ std::move(text) };
            }
            return std::nullopt;
        case sdk::StudioUiComponentValueType::Resource:
            if (value.is_string()) {
                auto text = value.get<std::string>();
                if (text.empty() || Uuid::Parse(text)) return sdk::StudioUiResourceValue{ std::move(text) };
            }
            if (value.is_object() && value.value("type", std::string{}) == "resource" && value.contains("value") && value["value"].is_string()) {
                auto text = value["value"].get<std::string>();
                if (text.empty() || Uuid::Parse(text)) return sdk::StudioUiResourceValue{ std::move(text) };
            }
            return std::nullopt;
        case sdk::StudioUiComponentValueType::Token:
            if (value.is_string()) {
                auto text = value.get<std::string>();
                if (!text.empty()) return sdk::StudioUiTokenValue{ std::move(text) };
            }
            if (value.is_object() && value.value("type", std::string{}) == "token" && value.contains("value") && value["value"].is_string()) {
                auto text = value["value"].get<std::string>();
                if (!text.empty()) return sdk::StudioUiTokenValue{ std::move(text) };
            }
            return std::nullopt;
        case sdk::StudioUiComponentValueType::Array:
            return value.is_array() && IsBoundedRuntimeJson(value) ? std::optional<sdk::StudioUiValue>{ sdk::StudioUiArrayValue{ value.dump() } } : std::nullopt;
        case sdk::StudioUiComponentValueType::Object:
            return value.is_object() && IsBoundedRuntimeJson(value) ? std::optional<sdk::StudioUiValue>{ sdk::StudioUiObjectValue{ value.dump() } } : std::nullopt;
    }
    return std::nullopt;
}

std::optional<double> ComponentNumber(const sdk::StudioUiValue& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) return static_cast<double>(*integer);
    if (const auto* number = std::get_if<double>(&value)) return *number;
    return std::nullopt;
}

std::optional<std::string_view> ComponentText(const sdk::StudioUiValue& value) {
    if (const auto* text = std::get_if<std::string>(&value)) return *text;
    if (const auto* uuid = std::get_if<sdk::StudioUiUuidValue>(&value)) return uuid->value;
    if (const auto* resource = std::get_if<sdk::StudioUiResourceValue>(&value)) return resource->value;
    if (const auto* token = std::get_if<sdk::StudioUiTokenValue>(&value)) return token->value;
    return std::nullopt;
}

bool ApplyComponentProperty(sdk::StudioUiNode& target, const std::string_view path, const sdk::StudioUiValue& value) {
    const auto text = ComponentText(value);
    const auto* resource = std::get_if<sdk::StudioUiResourceValue>(&value);
    const auto* boolean = std::get_if<bool>(&value);
    const auto number = ComponentNumber(value);
    const bool nullValue = std::holds_alternative<std::monostate>(value);
    if (path == "name" && text)
        target.name = *text;
    else if (path == "visible" && boolean)
        target.visible = *boolean;
    else if (path == "locked" && boolean)
        target.locked = *boolean;
    else if (path == "layout.x" && number)
        target.layout.x = static_cast<float>(*number);
    else if (path == "layout.y" && number)
        target.layout.y = static_cast<float>(*number);
    else if (path == "layout.width" && number)
        target.layout.width = static_cast<float>(*number);
    else if (path == "layout.height" && number)
        target.layout.height = static_cast<float>(*number);
    else if (path == "layout.anchorX" && number)
        target.layout.anchorX = static_cast<float>(*number);
    else if (path == "layout.anchorY" && number)
        target.layout.anchorY = static_cast<float>(*number);
    else if (path == "layout.anchorRight" && number)
        target.layout.anchorRight = static_cast<float>(*number);
    else if (path == "layout.anchorBottom" && number)
        target.layout.anchorBottom = static_cast<float>(*number);
    else if (path == "layout.pivotX" && number)
        target.layout.pivotX = static_cast<float>(*number);
    else if (path == "layout.pivotY" && number)
        target.layout.pivotY = static_cast<float>(*number);
    else if (path == "layout.margin" && number)
        target.layout.margin = static_cast<float>(*number);
    else if (path == "layout.alignment" && text)
        target.layout.alignment = *text;
    else if (path == "layout.sizeRule" && text)
        target.layout.sizeRule = *text;
    else if (path == "content.text" && text)
        target.text = *text;
    else if (path == "content.assetId" && resource && resource->value.empty())
        target.assetId.reset();
    else if (path == "content.assetId" && text)
        target.assetId = *text;
    else if (path == "content.assetId" && nullValue)
        target.assetId.reset();
    else if (path == "appearance.backgroundColor" && text)
        target.appearance.backgroundColor = *text;
    else if (path == "appearance.textColor" && text)
        target.appearance.textColor = *text;
    else if (path == "appearance.opacity" && number)
        target.appearance.opacity = static_cast<float>(*number);
    else if (path == "appearance.styleToken" && text)
        target.appearance.styleToken = *text;
    else if (path == "appearance.styleToken" && nullValue)
        target.appearance.styleToken.reset();
    else if (path == "appearance.hoverBackgroundColor" && text)
        target.appearance.hoverBackgroundColor = *text;
    else if (path == "appearance.hoverBackgroundColor" && nullValue)
        target.appearance.hoverBackgroundColor.reset();
    else if (path == "appearance.focusColor" && text)
        target.appearance.focusColor = *text;
    else if (path == "appearance.focusColor" && nullValue)
        target.appearance.focusColor.reset();
    else if (path == "appearance.disabledOpacity" && number)
        target.appearance.disabledOpacity = static_cast<float>(*number);
    else if (path == "accessibility.label" && text)
        target.accessibilityLabel = *text;
    else if (path == "accessibility.role" && text)
        target.accessibilityRole = *text;
    else if (path.starts_with("runtimeProperties.") && path.size() > std::string_view("runtimeProperties.").size()) {
        target.runtimeProperties[std::string(path.substr(std::string_view("runtimeProperties.").size()))] = value;
    }
    else
        return false;
    return true;
}

Result<sdk::StudioUiDocument> ResolveComponents(const sdk::StudioUiDocument& source, const StudioUiComponentLoader& loader, const std::string& scenePath) {
    sdk::StudioUiDocument document = source;
    const bool hasInstances = std::ranges::any_of(document.nodes, [](const sdk::StudioUiNode& node) { return node.componentInstance.has_value(); });
    if (!hasInstances) return Result<sdk::StudioUiDocument>::Success(std::move(document));
    std::unordered_map<std::string, const sdk::StudioUiNode*> sourceNodes;
    for (const auto& node : document.nodes) sourceNodes.emplace(node.id, &node);
    for (const auto& node : document.nodes) {
        if (node.componentInstance || node.componentSlot || !node.parentId) continue;
        const sdk::StudioUiNode* cursor = nullptr;
        if (const auto parent = sourceNodes.find(*node.parentId); parent != sourceNodes.end()) cursor = parent->second;
        std::unordered_set<std::string> visited;
        while (cursor && visited.insert(cursor->id).second) {
            if (cursor->componentInstance) return Result<sdk::StudioUiDocument>::Failure(ApplicationError("PXUISTUDIO2127", "Studio UI local component structure requires a declared named slot", scenePath, node.id));
            if (cursor->componentSlot || !cursor->parentId) break;
            const auto parent = sourceNodes.find(*cursor->parentId);
            cursor = parent == sourceNodes.end() ? nullptr : parent->second;
        }
    }
    if (!loader) return Result<sdk::StudioUiDocument>::Failure(ApplicationError("PXUISTUDIO2110", "Studio UI component instances require a production component loader", scenePath));

    constexpr std::size_t kMaxComponentDepth = 32;
    constexpr std::size_t kMaxComponentDocuments = 512;
    constexpr std::size_t kMaxComponentDocumentNodes = 4096;
    constexpr std::size_t kMaxComponentGraphNodes = 65'536;
    constexpr std::size_t kMaxComponentSourceBytes = 8 * 1024 * 1024;
    if (document.nodes.size() > kMaxComponentGraphNodes) return Result<sdk::StudioUiDocument>::Failure(ApplicationError("PXUISTUDIO2124", "Studio UI component projection exceeds the 65536 node budget", scenePath));

    struct CachedComponent {
        sdk::StudioUiComponentDocument document;
        std::string sourcePath;
    };
    std::unordered_map<std::string, CachedComponent> cache;
    std::unordered_set<std::string> active;
    std::unordered_set<std::string> discovered;
    std::size_t loadedNodeCount = document.nodes.size();
    std::vector<diag::Diagnostic> diagnostics;
    std::function<const CachedComponent*(const std::string&, std::size_t)> load;
    load = [&](const std::string& componentId, const std::size_t depth) -> const CachedComponent* {
        if (depth > kMaxComponentDepth) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2122", "Studio UI component dependency graph exceeds depth 32", scenePath));
            return nullptr;
        }
        if (active.contains(componentId)) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2113", "Studio UI component dependency cycle includes: " + componentId, scenePath));
            return nullptr;
        }
        if (const auto found = cache.find(componentId); found != cache.end()) return &found->second;
        if (discovered.insert(componentId).second && discovered.size() > kMaxComponentDocuments) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2123", "Studio UI component dependency graph exceeds 512 documents", scenePath));
            return nullptr;
        }
        const auto sourceDocument = loader(componentId);
        if (!sourceDocument) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2111", "Studio UI component source could not be loaded: " + componentId, scenePath));
            return nullptr;
        }
        if (sourceDocument->json.size() > kMaxComponentSourceBytes) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2125", "Studio UI component source exceeds the 8 MiB byte budget: " + componentId, sourceDocument->sourcePath));
            return nullptr;
        }
        const auto parsed = sdk::ParseStudioUiComponent(sourceDocument->json);
        if (!parsed.Valid()) {
            auto parsedDiagnostics = ComponentParseDiagnostics(parsed, sourceDocument->sourcePath);
            diagnostics.insert(diagnostics.end(), std::make_move_iterator(parsedDiagnostics.begin()), std::make_move_iterator(parsedDiagnostics.end()));
            return nullptr;
        }
        if (parsed.document.content.id != componentId) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2112", "Studio UI component source identity does not match request: " + componentId, sourceDocument->sourcePath));
            return nullptr;
        }
        if (parsed.document.content.nodes.size() > kMaxComponentDocumentNodes) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2124", "Studio UI component source exceeds the 4096 node document budget", sourceDocument->sourcePath));
            return nullptr;
        }
        if (parsed.document.content.nodes.size() > kMaxComponentGraphNodes - std::min(loadedNodeCount, kMaxComponentGraphNodes)) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2124", "Studio UI component dependency graph exceeds 65536 nodes", sourceDocument->sourcePath));
            return nullptr;
        }
        loadedNodeCount += parsed.document.content.nodes.size();
        active.insert(componentId);
        for (const auto& node : parsed.document.content.nodes) {
            if (node.componentInstance && node.id == node.componentInstance->instanceRootId) (void)load(node.componentInstance->componentId, depth + 1);
        }
        active.erase(componentId);
        if (!diagnostics.empty()) return nullptr;
        auto [inserted, unused] = cache.emplace(componentId, CachedComponent{ parsed.document, sourceDocument->sourcePath });
        (void)unused;
        return &inserted->second;
    };

    using RuntimeMapping = std::unordered_map<std::string, sdk::StudioUiNode*>;
    const auto completeProjection = [&](const CachedComponent& component, const RuntimeMapping& mapped, const std::string& runtimeRootId) {
        bool complete = true;
        if (mapped.size() != component.document.content.nodes.size()) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2114", "Studio UI component instance is not a complete source projection", scenePath, runtimeRootId));
            complete = false;
        }
        for (const auto& sourceNode : component.document.content.nodes) {
            if (!mapped.contains(sourceNode.id)) {
                diagnostics.push_back(ApplicationError("PXUISTUDIO2114", "Studio UI component instance is missing source node: " + sourceNode.id, scenePath, runtimeRootId));
                complete = false;
            }
        }
        const auto root = mapped.find(component.document.content.rootId);
        if (root == mapped.end() || root->second->id != runtimeRootId) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2114", "Studio UI component root projection lost its stable identity", scenePath, runtimeRootId));
            complete = false;
        }
        return complete;
    };

    const auto applyPublicApi = [&](const CachedComponent& component, const sdk::StudioUiComponentInstance& instance, const RuntimeMapping& mapped, const std::string& runtimeRootId) {
        std::unordered_map<std::string, const sdk::StudioUiComponentProperty*> properties;
        for (const auto& property : component.document.componentInterface.properties) properties.emplace(property.id, &property);
        for (const auto& [propertyId, authored] : instance.publicProperties) {
            const auto property = properties.find(propertyId);
            if (property == properties.end()) {
                diagnostics.push_back(ApplicationError("PXUISTUDIO2115", "Studio UI component instance references an undeclared public property: " + propertyId, scenePath, runtimeRootId));
                continue;
            }
            const auto value = ComponentValue(property->second->valueType, authored);
            const auto target = mapped.find(property->second->nodeId);
            if (!value || target == mapped.end() || !ApplyComponentProperty(*target->second, property->second->property, *value)) {
                diagnostics.push_back(ApplicationError("PXUISTUDIO2117", "Studio UI public property cannot be applied to its declared target: " + propertyId, scenePath, runtimeRootId, property->second->property));
            }
        }

        std::unordered_map<std::string, const sdk::StudioUiComponentSignal*> signals;
        for (const auto& signal : component.document.componentInterface.signals) signals.emplace(signal.id, &signal);
        for (const auto& [signalId, action] : instance.publicSignals) {
            const auto signal = signals.find(signalId);
            if (signal == signals.end()) {
                diagnostics.push_back(ApplicationError("PXUISTUDIO2118", "Studio UI component instance references an undeclared public signal: " + signalId, scenePath, runtimeRootId));
                continue;
            }
            if (!action) continue;
            const auto target = mapped.find(signal->second->nodeId);
            std::unordered_set<std::string> arguments;
            for (const auto& argument : signal->second->arguments) arguments.insert(argument.id);
            const bool mappingValid = std::ranges::all_of(action->argumentBindings, [&](const auto& mapping) { return arguments.contains(mapping.second); });
            if (target == mapped.end() || !mappingValid) {
                diagnostics.push_back(ApplicationError("PXUISTUDIO2119", "Studio UI public signal target or argument mapping is invalid", scenePath, runtimeRootId));
                continue;
            }
            target->second->resolvedSignalActions.push_back({ signal->second->signal, action->action, signal->second->arguments, action->argumentBindings });
        }

        std::unordered_map<std::string, const sdk::StudioUiComponentSlotDefinition*> slots;
        for (const auto& slot : component.document.componentInterface.slots) slots.emplace(slot.id, &slot);
        for (const auto& slotNode : document.nodes) {
            if (!slotNode.componentSlot || slotNode.componentSlot->instanceRootId != runtimeRootId) continue;
            const auto slot = slots.find(slotNode.componentSlot->slotId);
            if (slot == slots.end()) {
                diagnostics.push_back(ApplicationError("PXUISTUDIO2120", "Studio UI component instance references an undeclared slot: " + slotNode.componentSlot->slotId, scenePath, slotNode.id));
                continue;
            }
            const auto target = mapped.find(slot->second->nodeId);
            if (target == mapped.end() || !slotNode.parentId || *slotNode.parentId != target->second->id) {
                diagnostics.push_back(ApplicationError("PXUISTUDIO2121", "Studio UI slot content is not projected to its declared target", scenePath, slotNode.id));
            }
        }
    };

    std::function<void(const CachedComponent&, const sdk::StudioUiComponentInstance&, const RuntimeMapping&, const std::string&, std::size_t)> applyInstance;
    applyInstance = [&](const CachedComponent& component, const sdk::StudioUiComponentInstance& instance, const RuntimeMapping& mapped, const std::string& runtimeRootId, const std::size_t depth) {
        if (depth > kMaxComponentDepth) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2122", "Studio UI component application exceeds depth 32", scenePath, runtimeRootId));
            return;
        }
        if (!completeProjection(component, mapped, runtimeRootId)) return;
        applyPublicApi(component, instance, mapped, runtimeRootId);

        for (const auto& nestedRoot : component.document.content.nodes) {
            if (!nestedRoot.componentInstance || nestedRoot.id != nestedRoot.componentInstance->instanceRootId) continue;
            const auto& nestedInstance = *nestedRoot.componentInstance;
            const CachedComponent* nested = load(nestedInstance.componentId, depth + 1);
            if (!nested) continue;
            RuntimeMapping nestedMapped;
            bool nestedPathsValid = true;
            for (const auto& sourceNode : component.document.content.nodes) {
                if (!sourceNode.componentInstance || sourceNode.componentInstance->instanceRootId != nestedRoot.id || sourceNode.componentInstance->componentId != nestedInstance.componentId) continue;
                const auto runtimeNode = mapped.find(sourceNode.id);
                if (runtimeNode == mapped.end()) continue;
                const auto nestedSourceNode = std::ranges::find_if(nested->document.content.nodes, [&](const sdk::StudioUiNode& candidate) { return candidate.id == sourceNode.componentInstance->sourceNodeId; });
                std::vector<std::string> expected{ nestedInstance.componentId };
                if (nestedSourceNode != nested->document.content.nodes.end() && nestedSourceNode->componentInstance) {
                    expected.push_back(nestedSourceNode->componentInstance->instanceRootId);
                    expected.push_back(nestedSourceNode->componentInstance->componentId);
                    expected.push_back(nestedSourceNode->componentInstance->sourceNodeId);
                }
                else {
                    expected.push_back(sourceNode.componentInstance->sourceNodeId);
                }
                if (sourceNode.componentInstance->sourcePath != expected) {
                    diagnostics.push_back(ApplicationError("PXUISTUDIO2126", "Nested Studio UI sourcePath does not match its UUID-authoritative source projection", component.sourcePath, sourceNode.id));
                    nestedPathsValid = false;
                }
                nestedMapped.emplace(sourceNode.componentInstance->sourceNodeId, runtimeNode->second);
            }
            const auto nestedRuntimeRoot = mapped.find(nestedRoot.id);
            if (nestedRuntimeRoot == mapped.end()) {
                diagnostics.push_back(ApplicationError("PXUISTUDIO2114", "Nested Studio UI component root is missing from the Runtime projection", scenePath, runtimeRootId));
                continue;
            }
            if (!nestedPathsValid) continue;
            applyInstance(*nested, nestedInstance, nestedMapped, nestedRuntimeRoot->second->id, depth + 1);
        }
    };

    for (auto& root : document.nodes) {
        if (!root.componentInstance || root.id != root.componentInstance->instanceRootId) continue;
        const auto& instance = *root.componentInstance;
        const CachedComponent* component = load(instance.componentId, 1);
        if (!component) continue;
        RuntimeMapping mapped;
        for (auto& candidate : document.nodes) {
            if (candidate.componentInstance && candidate.componentInstance->instanceRootId == root.id && candidate.componentInstance->componentId == instance.componentId) mapped.emplace(candidate.componentInstance->sourceNodeId, &candidate);
        }
        if (!completeProjection(*component, mapped, root.id)) continue;
        bool pathsValid = true;
        for (const auto& sourceNode : component->document.content.nodes) {
            const auto runtimeNode = mapped.find(sourceNode.id);
            if (runtimeNode == mapped.end() || !runtimeNode->second->componentInstance) continue;
            std::vector<std::string> expected{ instance.componentId };
            if (sourceNode.componentInstance) {
                expected.push_back(sourceNode.componentInstance->instanceRootId);
                expected.push_back(sourceNode.componentInstance->componentId);
                expected.push_back(sourceNode.componentInstance->sourceNodeId);
            }
            else {
                expected.push_back(sourceNode.id);
            }
            if (runtimeNode->second->componentInstance->sourcePath != expected) {
                diagnostics.push_back(ApplicationError("PXUISTUDIO2126", "Studio UI component sourcePath does not match the UUID-authoritative source projection", scenePath, runtimeNode->second->id));
                pathsValid = false;
            }
        }
        if (pathsValid) applyInstance(*component, instance, mapped, root.id, 1);
    }
    if (!diagnostics.empty()) return Result<sdk::StudioUiDocument>::Failure(std::move(diagnostics));
    return Result<sdk::StudioUiDocument>::Success(std::move(document));
}

}  // namespace

Result<StudioUiApplicationSummary> StudioUiApplication::ApplyText(const std::string_view json, StudioUiApplicationOptions options) {
    const auto parsed = sdk::ParseStudioUi(json);
    if (!parsed.Valid()) {
        return Result<StudioUiApplicationSummary>::Failure(ParseDiagnostics(parsed, options.sourcePath));
    }
    return ApplyDocument(parsed.document, std::move(options));
}

Result<StudioUiApplicationSummary> StudioUiApplication::ApplyDocument(const sdk::StudioUiDocument& document, StudioUiApplicationOptions options) {
    if (const auto registered = RegisterBuiltinUITypes(); !registered) {
        return Result<StudioUiApplicationSummary>::Failure(registered.Diagnostics());
    }

    auto resolved = ResolveComponents(document, options.loadComponent, options.sourcePath);
    if (!resolved) return Result<StudioUiApplicationSummary>::Failure(resolved.Diagnostics());
    sdk::StudioUiDocument resolvedDocument = std::move(resolved.Value());

    UIContext* context = &m_context;
    const std::string sourcePath = options.sourcePath;
    const bool previewSafeMode = options.previewSafeMode;
    auto observer = std::move(options.observeAction);
    auto runtimeTree =
        BuildStudioUiRuntimeTree(resolvedDocument, std::move(options.resolveAsset), [context, sourcePath, previewSafeMode, observer = std::move(observer)](const sdk::StudioUiAction& action, const std::string_view signal, const std::string_view nodeId) {
            ActionInvocation invocation;
            invocation.action = action.id;
            invocation.context.preview = previewSafeMode;
            invocation.context.sourceScene = sourcePath;
            invocation.context.signal = "studioUi." + std::string(signal);
            if (const auto sourceNode = Uuid::Parse(nodeId)) invocation.context.sourceNode = *sourceNode;
            for (const auto& [name, source] : action.arguments) {
                auto value = RuntimeActionValue(source);
                if (!value) {
                    const Status status = Status::Fail(ApplicationError(
                        "PXUISTUDIO2105",
                        "Studio UI Action argument cannot be represented by "
                        "the Runtime: " +
                            name,
                        sourcePath,
                        {},
                        "interaction.onClick.arguments." + name
                    ));
                    for (const auto& diagnostic : status.Diagnostics()) diag::Emit(diagnostic);
                    if (observer) observer(action, status);
                    return;
                }
                invocation.arguments.emplace(name, std::move(*value));
            }
            const Status status = context->Actions().Dispatch(std::move(invocation), { .previewSafeMode = previewSafeMode });
            if (observer) observer(action, status);
        });

    std::vector<diag::Diagnostic> diagnostics;
    for (const auto& source : runtimeTree.diagnostics) {
        diagnostics.push_back(ApplicationError(source.code, source.message, sourcePath, source.nodeId));
    }
    for (const auto& assetId : runtimeTree.unresolvedAssetIds) {
        diagnostics.push_back(ApplicationError("PXUISTUDIO2004", "Studio UI image asset could not be resolved: " + assetId, sourcePath, {}, "content.assetId"));
    }
    if (!runtimeTree.root && diagnostics.empty()) {
        diagnostics.push_back(ApplicationError("PXUISTUDIO2101", "Studio UI did not produce a Runtime root", sourcePath));
    }
    if (!diagnostics.empty()) return Result<StudioUiApplicationSummary>::Failure(std::move(diagnostics));

    std::vector<Binding> propertyBindings;
    for (const auto& node : resolvedDocument.nodes) {
        if (node.bindings.empty()) continue;
        if (!options.viewModel) {
            return Result<StudioUiApplicationSummary>::Failure(ApplicationError("PXUISTUDIO2106", "Studio UI contains property bindings but no ViewModel was provided", sourcePath, node.id, "bindings"));
        }
        const auto nodeId = Uuid::Parse(node.id);
        auto* target = nodeId ? dynamic_cast<Control*>(runtimeTree.root->Find(*nodeId)) : nullptr;
        if (!target) {
            return Result<StudioUiApplicationSummary>::Failure(ApplicationError("PXUISTUDIO2107", "Studio UI property binding target control is missing", sourcePath, node.id, "bindings"));
        }
        for (const auto& [targetName, descriptor] : node.bindings) {
            const auto* property = TypeRegistry::Global().FindProperty(std::string(target->TypeName()), targetName);
            if (!property || !property->set || !property->bindable) {
                return Result<StudioUiApplicationSummary>::Failure(ApplicationError("PXUISTUDIO2108", "Studio UI binding target is not a bindable Runtime property: " + targetName, sourcePath, node.id, "bindings." + targetName));
            }
            BindingTarget bindingTarget{ property->type, node.name + "." + targetName, [target, property](const Variant& value) { return property->set(*target, value); } };
            auto binding = Binding::Create(*options.viewModel, descriptor.path, std::move(bindingTarget), m_context.Formatters(), descriptor.formatter);
            if (!binding) return Result<StudioUiApplicationSummary>::Failure(binding.Diagnostics());
            propertyBindings.push_back(std::move(binding.Value()));
        }
    }

    if (runtimeTree.animations) {
        for (const auto& clip : runtimeTree.animations->clips) {
            for (const auto& track : clip.tracks) {
                auto* target = runtimeTree.root->Find(track.node);
                const auto* property = target ? TypeRegistry::Global().FindProperty(std::string(target->TypeName()), track.property) : nullptr;
                if (!target || !property || !property->get || !property->set) {
                    return Result<StudioUiApplicationSummary>::Failure(ApplicationError(
                        "PXUISTUDIO2102",
                        "Animation target is not a writable Runtime "
                        "property: " +
                            track.node.ToString() + "." + track.property,
                        sourcePath,
                        track.node.ToString(),
                        track.property
                    ));
                }
            }
        }
    }

    StudioUiApplicationSummary summary{ .documentId = resolvedDocument.id,
                                        .revision = resolvedDocument.revision,
                                        .nodeCount = runtimeTree.nodeCount,
                                        .actionBindingCount = runtimeTree.actionBindingCount,
                                        .behaviorNodeCount = runtimeTree.behaviorNodeCount,
                                        .behaviorTriggerCount = runtimeTree.behaviorTriggerCount,
                                        .animationClipCount = runtimeTree.animationClipCount,
                                        .animationTrackCount = runtimeTree.animationTrackCount,
                                        .propertyBindingCount = propertyBindings.size() };

    if (const auto installed = m_context.SetRoot(std::move(runtimeTree.root)); !installed) {
        return Result<StudioUiApplicationSummary>::Failure(installed.Diagnostics());
    }
    if (runtimeTree.animations) {
        if (const auto installed = m_context.SetAnimations(std::move(*runtimeTree.animations), true); !installed) {
            return Result<StudioUiApplicationSummary>::Failure(installed.Diagnostics());
        }
    }
    if (runtimeTree.behaviorGraph || !runtimeTree.behaviorTriggers.empty()) {
        if (const auto configured = m_context.ConfigureTriggers(std::move(runtimeTree.behaviorTriggers), std::move(runtimeTree.behaviorGraph), sourcePath); !configured) {
            return Result<StudioUiApplicationSummary>::Failure(configured.Diagnostics());
        }
    }
    m_context.SetBindings(std::move(propertyBindings));
    m_context.SetDiagnosticOverlayEnabled(options.diagnosticOverlay);
    return Result<StudioUiApplicationSummary>::Success(std::move(summary));
}

Result<StudioUiApplicationSummary> StudioUiApplication::PatchDocumentProperties(const sdk::StudioUiDocument& document, StudioUiApplicationOptions options) {
    if (const auto registered = RegisterBuiltinUITypes(); !registered) {
        return Result<StudioUiApplicationSummary>::Failure(registered.Diagnostics());
    }
    auto* installedRoot = m_context.Root();
    if (!installedRoot) {
        return Result<StudioUiApplicationSummary>::Failure(ApplicationError("PXUISTUDIO2130", "Studio UI property patch requires an installed Runtime root", options.sourcePath));
    }

    auto resolved = ResolveComponents(document, options.loadComponent, options.sourcePath);
    if (!resolved) return Result<StudioUiApplicationSummary>::Failure(resolved.Diagnostics());
    sdk::StudioUiDocument resolvedDocument = std::move(resolved.Value());
    auto candidate = BuildStudioUiRuntimeTree(resolvedDocument, std::move(options.resolveAsset));

    std::vector<diag::Diagnostic> diagnostics;
    for (const auto& source : candidate.diagnostics) {
        diagnostics.push_back(ApplicationError(source.code, source.message, options.sourcePath, source.nodeId));
    }
    for (const auto& assetId : candidate.unresolvedAssetIds) {
        diagnostics.push_back(ApplicationError("PXUISTUDIO2004", "Studio UI image asset could not be resolved: " + assetId, options.sourcePath, {}, "content.assetId"));
    }
    if (!candidate.root && diagnostics.empty()) {
        diagnostics.push_back(ApplicationError("PXUISTUDIO2131", "Studio UI property patch did not produce a candidate Runtime root", options.sourcePath));
    }
    if (!diagnostics.empty()) return Result<StudioUiApplicationSummary>::Failure(std::move(diagnostics));

    for (const auto& node : resolvedDocument.nodes) {
        const auto nodeId = Uuid::Parse(node.id);
        auto* installed = nodeId ? dynamic_cast<Control*>(installedRoot->Find(*nodeId)) : nullptr;
        const auto* source = nodeId ? dynamic_cast<const Control*>(candidate.root->Find(*nodeId)) : nullptr;
        if (!installed || !source || installed->TypeName() != source->TypeName()) {
            return Result<StudioUiApplicationSummary>::Failure(ApplicationError("PXUISTUDIO2132", "Studio UI property patch topology does not match the installed Runtime tree", options.sourcePath, node.id));
        }
        installed->SetName(source->Name());
        installed->SetStyleToken(source->StyleToken());
        for (const auto* property : TypeRegistry::Global().PropertiesForType(std::string(source->TypeName()))) {
            if (!property || !property->get || !property->set) continue;
            const Variant value = property->get(*source);
            if (property->get(*installed) == value) continue;
            if (const auto status = property->set(*installed, value); !status) {
                return Result<StudioUiApplicationSummary>::Failure(ApplicationError("PXUISTUDIO2133", "Studio UI Runtime property patch failed: " + property->name, options.sourcePath, node.id, property->name));
            }
        }
    }

    std::size_t propertyBindingCount = 0;
    for (const auto& node : resolvedDocument.nodes) propertyBindingCount += node.bindings.size();
    m_context.SetDiagnosticOverlayEnabled(options.diagnosticOverlay);
    return Result<StudioUiApplicationSummary>::Success(
        { .documentId = resolvedDocument.id,
          .revision = resolvedDocument.revision,
          .nodeCount = candidate.nodeCount,
          .actionBindingCount = candidate.actionBindingCount,
          .behaviorNodeCount = candidate.behaviorNodeCount,
          .behaviorTriggerCount = candidate.behaviorTriggerCount,
          .animationClipCount = candidate.animationClipCount,
          .animationTrackCount = candidate.animationTrackCount,
          .propertyBindingCount = propertyBindingCount }
    );
}

}  // namespace px::ui
