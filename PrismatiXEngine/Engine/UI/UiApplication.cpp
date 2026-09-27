#include "Engine/UI/UiApplication.h"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Engine/Core/TypeRegistry.h"
#include "Engine/Core/Uuid.h"
#include "Engine/UI/Binding.h"
#include "Engine/UI/UIContext.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Engine/UI/VirtualizedView.h"
#include "Engine/UI/Widgets.h"

namespace px::ui {
namespace {

using Json = nlohmann::json;

diag::Diagnostic ApplicationError(std::string code, std::string message, const std::string& sourcePath, std::string nodeId = {}, std::string property = {}) {
    diag::Diagnostic diagnostic{ .severity = diag::Severity::Error, .code = std::move(code), .category = "UI.Application", .message = std::move(message) };
    diagnostic.source.path = sourcePath;
    diagnostic.source.nodeId = std::move(nodeId);
    diagnostic.source.property = std::move(property);
    return diagnostic;
}

std::vector<diag::Diagnostic> ParseDiagnostics(const sdk::UiParseResult& parsed, const std::string& sourcePath) {
    std::vector<diag::Diagnostic> diagnostics;
    diagnostics.reserve(parsed.diagnostics.size());
    for (const auto& source : parsed.diagnostics) {
        auto diagnostic = ApplicationError(source.code, source.message, sourcePath);
        diagnostic.details = "nodeIndex=" + std::to_string(source.nodeIndex);
        diagnostics.push_back(std::move(diagnostic));
    }
    return diagnostics;
}

std::vector<diag::Diagnostic> ComponentParseDiagnostics(const sdk::UiComponentParseResult& parsed, const std::string& sourcePath) {
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

std::optional<sdk::UiValue> ComponentValue(const sdk::UiComponentValueType type, const sdk::UiComponentPublicValue& source) {
    constexpr std::size_t kMaxJsonBytes = 1024 * 1024;
    if (source.json.size() > kMaxJsonBytes) return std::nullopt;
    const Json value = Json::parse(source.json, nullptr, false);
    if (value.is_discarded()) return std::nullopt;
    switch (type) {
        case sdk::UiComponentValueType::Null:
            return value.is_null() ? std::optional<sdk::UiValue>{ std::monostate{} } : std::nullopt;
        case sdk::UiComponentValueType::Boolean:
            return value.is_boolean() ? std::optional<sdk::UiValue>{ value.get<bool>() } : std::nullopt;
        case sdk::UiComponentValueType::Integer:
            return value.is_number_integer() ? std::optional<sdk::UiValue>{ value.get<std::int64_t>() } : std::nullopt;
        case sdk::UiComponentValueType::Number:
            return value.is_number() ? std::optional<sdk::UiValue>{ value.get<double>() } : std::nullopt;
        case sdk::UiComponentValueType::String:
            return value.is_string() ? std::optional<sdk::UiValue>{ value.get<std::string>() } : std::nullopt;
        case sdk::UiComponentValueType::Vec2:
            if (value.is_object() && value.value("type", std::string{}) == "vec2" && value.contains("x") && value["x"].is_number() && value.contains("y") && value["y"].is_number())
                return sdk::UiVec2Value{ value["x"].get<float>(), value["y"].get<float>() };
            return std::nullopt;
        case sdk::UiComponentValueType::Rect:
            if (value.is_object() && value.value("type", std::string{}) == "rect" && value.contains("x") && value["x"].is_number() && value.contains("y") && value["y"].is_number() && value.contains("width") && value["width"].is_number() &&
                value.contains("height") && value["height"].is_number())
                return sdk::UiRectValue{ value["x"].get<float>(), value["y"].get<float>(), value["width"].get<float>(), value["height"].get<float>() };
            return std::nullopt;
        case sdk::UiComponentValueType::Color:
            if (value.is_object() && value.value("type", std::string{}) == "color" && value.contains("value") && value["value"].is_string()) return sdk::UiColorValue{ value["value"].get<std::string>() };
            return std::nullopt;
        case sdk::UiComponentValueType::Uuid:
            if (value.is_string()) {
                auto text = value.get<std::string>();
                if (Uuid::Parse(text)) return sdk::UiUuidValue{ std::move(text) };
            }
            return std::nullopt;
        case sdk::UiComponentValueType::Resource:
            if (value.is_string()) {
                auto text = value.get<std::string>();
                if (text.empty() || Uuid::Parse(text)) return sdk::UiResourceValue{ std::move(text) };
            }
            if (value.is_object() && value.value("type", std::string{}) == "resource" && value.contains("value") && value["value"].is_string()) {
                auto text = value["value"].get<std::string>();
                if (text.empty() || Uuid::Parse(text)) return sdk::UiResourceValue{ std::move(text) };
            }
            return std::nullopt;
        case sdk::UiComponentValueType::Token:
            if (value.is_string()) {
                auto text = value.get<std::string>();
                if (!text.empty()) return sdk::UiTokenValue{ std::move(text) };
            }
            if (value.is_object() && value.value("type", std::string{}) == "token" && value.contains("value") && value["value"].is_string()) {
                auto text = value["value"].get<std::string>();
                if (!text.empty()) return sdk::UiTokenValue{ std::move(text) };
            }
            return std::nullopt;
        case sdk::UiComponentValueType::Array:
            return value.is_array() && IsBoundedRuntimeJson(value) ? std::optional<sdk::UiValue>{ sdk::UiArrayValue{ value.dump() } } : std::nullopt;
        case sdk::UiComponentValueType::Object:
            return value.is_object() && IsBoundedRuntimeJson(value) ? std::optional<sdk::UiValue>{ sdk::UiObjectValue{ value.dump() } } : std::nullopt;
    }
    return std::nullopt;
}

std::optional<double> ComponentNumber(const sdk::UiValue& value) {
    if (const auto* integer = std::get_if<std::int64_t>(&value)) return static_cast<double>(*integer);
    if (const auto* number = std::get_if<double>(&value)) return *number;
    return std::nullopt;
}

std::optional<std::string_view> ComponentText(const sdk::UiValue& value) {
    if (const auto* text = std::get_if<std::string>(&value)) return *text;
    if (const auto* uuid = std::get_if<sdk::UiUuidValue>(&value)) return uuid->value;
    if (const auto* resource = std::get_if<sdk::UiResourceValue>(&value)) return resource->value;
    if (const auto* token = std::get_if<sdk::UiTokenValue>(&value)) return token->value;
    return std::nullopt;
}

bool ApplyComponentProperty(sdk::UiNode& target, const std::string_view path, const sdk::UiValue& value) {
    const auto text = ComponentText(value);
    const auto* resource = std::get_if<sdk::UiResourceValue>(&value);
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
    else if (path == "text" && text)
        target.text = *text;
    else if (path == "assetId" && resource && resource->value.empty())
        target.assetId.reset();
    else if (path == "assetId" && text)
        target.assetId = *text;
    else if (path == "assetId" && nullValue)
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
    else if (path == "accessibilityLabel" && text)
        target.accessibilityLabel = *text;
    else if (path == "accessibilityRole" && text)
        target.accessibilityRole = *text;
    else if (path == "accessibilityDescription" && text)
        target.accessibilityDescription = *text;
    else if (path == "accessibilityFocusOrder") {
        const auto* integer = std::get_if<std::int64_t>(&value);
        if (!integer || *integer < -1000000 || *integer > 1000000) return false;
        target.accessibilityFocusOrder = static_cast<std::int32_t>(*integer);
    }
    else if (path.starts_with("runtimeProperties.") && path.size() > std::string_view("runtimeProperties.").size()) {
        target.runtimeProperties[std::string(path.substr(std::string_view("runtimeProperties.").size()))] = value;
    }
    else
        return false;
    return true;
}

Result<sdk::UiDocument> ResolveComponents(const sdk::UiDocument& source, const UiComponentLoader& loader, const std::string& scenePath) {
    sdk::UiDocument document = source;
    const bool hasInstances = std::ranges::any_of(document.nodes, [](const sdk::UiNode& node) { return node.componentInstance.has_value(); });
    if (!hasInstances) return Result<sdk::UiDocument>::Success(std::move(document));
    std::unordered_map<std::string, const sdk::UiNode*> sourceNodes;
    for (const auto& node : document.nodes) sourceNodes.emplace(node.id, &node);
    for (const auto& node : document.nodes) {
        if (node.componentInstance || node.componentSlot || !node.parentId) continue;
        const sdk::UiNode* cursor = nullptr;
        if (const auto parent = sourceNodes.find(*node.parentId); parent != sourceNodes.end()) cursor = parent->second;
        std::unordered_set<std::string> visited;
        while (cursor && visited.insert(cursor->id).second) {
            if (cursor->componentInstance) return Result<sdk::UiDocument>::Failure(ApplicationError("PXUISTUDIO2127", "UI document local component structure requires a declared named slot", scenePath, node.id));
            if (cursor->componentSlot || !cursor->parentId) break;
            const auto parent = sourceNodes.find(*cursor->parentId);
            cursor = parent == sourceNodes.end() ? nullptr : parent->second;
        }
    }
    if (!loader) return Result<sdk::UiDocument>::Failure(ApplicationError("PXUISTUDIO2110", "UI component instances require a production component loader", scenePath));

    constexpr std::size_t kMaxComponentDepth = 32;
    constexpr std::size_t kMaxComponentDocuments = 512;
    constexpr std::size_t kMaxComponentDocumentNodes = 4096;
    constexpr std::size_t kMaxComponentGraphNodes = 65'536;
    constexpr std::size_t kMaxComponentSourceBytes = 8 * 1024 * 1024;
    if (document.nodes.size() > kMaxComponentGraphNodes) return Result<sdk::UiDocument>::Failure(ApplicationError("PXUISTUDIO2124", "UI component projection exceeds the 65536 node budget", scenePath));

    struct CachedComponent {
        sdk::UiComponentDocument document;
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
            diagnostics.push_back(ApplicationError("PXUISTUDIO2122", "UI component dependency graph exceeds depth 32", scenePath));
            return nullptr;
        }
        if (active.contains(componentId)) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2113", "UI component dependency cycle includes: " + componentId, scenePath));
            return nullptr;
        }
        if (const auto found = cache.find(componentId); found != cache.end()) return &found->second;
        if (discovered.insert(componentId).second && discovered.size() > kMaxComponentDocuments) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2123", "UI component dependency graph exceeds 512 documents", scenePath));
            return nullptr;
        }
        const auto sourceDocument = loader(componentId);
        if (!sourceDocument) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2111", "UI component source could not be loaded: " + componentId, scenePath));
            return nullptr;
        }
        if (sourceDocument->json.size() > kMaxComponentSourceBytes) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2125", "UI component source exceeds the 8 MiB byte budget: " + componentId, sourceDocument->sourcePath));
            return nullptr;
        }
        const auto parsed = sdk::ParseUiComponent(sourceDocument->json);
        if (!parsed.Valid()) {
            auto parsedDiagnostics = ComponentParseDiagnostics(parsed, sourceDocument->sourcePath);
            diagnostics.insert(diagnostics.end(), std::make_move_iterator(parsedDiagnostics.begin()), std::make_move_iterator(parsedDiagnostics.end()));
            return nullptr;
        }
        if (parsed.document.content.id != componentId) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2112", "UI component source identity does not match request: " + componentId, sourceDocument->sourcePath));
            return nullptr;
        }
        if (parsed.document.content.nodes.size() > kMaxComponentDocumentNodes) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2124", "UI component source exceeds the 4096 node document budget", sourceDocument->sourcePath));
            return nullptr;
        }
        if (parsed.document.content.nodes.size() > kMaxComponentGraphNodes - std::min(loadedNodeCount, kMaxComponentGraphNodes)) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2124", "UI component dependency graph exceeds 65536 nodes", sourceDocument->sourcePath));
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

    using RuntimeMapping = std::unordered_map<std::string, sdk::UiNode*>;
    const auto completeProjection = [&](const CachedComponent& component, const RuntimeMapping& mapped, const std::string& runtimeRootId) {
        bool complete = true;
        if (mapped.size() != component.document.content.nodes.size()) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2114", "UI component instance is not a complete source projection", scenePath, runtimeRootId));
            complete = false;
        }
        for (const auto& sourceNode : component.document.content.nodes) {
            if (!mapped.contains(sourceNode.id)) {
                diagnostics.push_back(ApplicationError("PXUISTUDIO2114", "UI component instance is missing source node: " + sourceNode.id, scenePath, runtimeRootId));
                complete = false;
            }
        }
        const auto root = mapped.find(component.document.content.rootId);
        if (root == mapped.end() || root->second->id != runtimeRootId) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2114", "UI component root projection lost its stable identity", scenePath, runtimeRootId));
            complete = false;
        }
        return complete;
    };

    const auto applyPublicApi = [&](const CachedComponent& component, const sdk::UiComponentInstance& instance, const RuntimeMapping& mapped, const std::string& runtimeRootId) {
        std::unordered_map<std::string, const sdk::UiComponentProperty*> properties;
        for (const auto& property : component.document.componentInterface.properties) properties.emplace(property.id, &property);
        for (const auto& [propertyId, authored] : instance.publicProperties) {
            const auto property = properties.find(propertyId);
            if (property == properties.end()) {
                diagnostics.push_back(ApplicationError("PXUISTUDIO2115", "UI component instance references an undeclared public property: " + propertyId, scenePath, runtimeRootId));
                continue;
            }
            const auto value = ComponentValue(property->second->valueType, authored);
            const auto target = mapped.find(property->second->nodeId);
            if (!value || target == mapped.end() || !ApplyComponentProperty(*target->second, property->second->property, *value)) {
                diagnostics.push_back(ApplicationError("PXUISTUDIO2117", "UI document public property cannot be applied to its declared target: " + propertyId, scenePath, runtimeRootId, property->second->property));
            }
        }

        std::unordered_map<std::string, const sdk::UiComponentSignal*> signals;
        for (const auto& signal : component.document.componentInterface.signals) signals.emplace(signal.id, &signal);
        for (const auto& [signalId, action] : instance.publicSignals) {
            const auto signal = signals.find(signalId);
            if (signal == signals.end()) {
                diagnostics.push_back(ApplicationError("PXUISTUDIO2118", "UI component instance references an undeclared public signal: " + signalId, scenePath, runtimeRootId));
                continue;
            }
            if (!action) continue;
            const auto target = mapped.find(signal->second->nodeId);
            std::unordered_set<std::string> arguments;
            for (const auto& argument : signal->second->arguments) arguments.insert(argument.id);
            const bool mappingValid = std::ranges::all_of(action->argumentBindings, [&](const auto& mapping) { return arguments.contains(mapping.second); });
            if (target == mapped.end() || !mappingValid) {
                diagnostics.push_back(ApplicationError("PXUISTUDIO2119", "UI document public signal target or argument mapping is invalid", scenePath, runtimeRootId));
                continue;
            }
            target->second->resolvedSignalActions.push_back({ signal->second->signal, action->action, signal->second->arguments, action->argumentBindings });
        }

        std::unordered_map<std::string, const sdk::UiComponentSlotDefinition*> slots;
        for (const auto& slot : component.document.componentInterface.slots) slots.emplace(slot.id, &slot);
        for (const auto& slotNode : document.nodes) {
            if (!slotNode.componentSlot || slotNode.componentSlot->instanceRootId != runtimeRootId) continue;
            const auto slot = slots.find(slotNode.componentSlot->slotId);
            if (slot == slots.end()) {
                diagnostics.push_back(ApplicationError("PXUISTUDIO2120", "UI component instance references an undeclared slot: " + slotNode.componentSlot->slotId, scenePath, slotNode.id));
                continue;
            }
            const auto target = mapped.find(slot->second->nodeId);
            if (target == mapped.end() || !slotNode.parentId || *slotNode.parentId != target->second->id) {
                diagnostics.push_back(ApplicationError("PXUISTUDIO2121", "UI document slot content is not projected to its declared target", scenePath, slotNode.id));
            }
        }
    };

    std::function<void(const CachedComponent&, const sdk::UiComponentInstance&, const RuntimeMapping&, const std::string&, std::size_t)> applyInstance;
    applyInstance = [&](const CachedComponent& component, const sdk::UiComponentInstance& instance, const RuntimeMapping& mapped, const std::string& runtimeRootId, const std::size_t depth) {
        if (depth > kMaxComponentDepth) {
            diagnostics.push_back(ApplicationError("PXUISTUDIO2122", "UI component application exceeds depth 32", scenePath, runtimeRootId));
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
                const auto nestedSourceNode = std::ranges::find_if(nested->document.content.nodes, [&](const sdk::UiNode& candidate) { return candidate.id == sourceNode.componentInstance->sourceNodeId; });
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
                    diagnostics.push_back(ApplicationError("PXUISTUDIO2126", "Nested UI document sourcePath does not match its UUID-authoritative source projection", component.sourcePath, sourceNode.id));
                    nestedPathsValid = false;
                }
                nestedMapped.emplace(sourceNode.componentInstance->sourceNodeId, runtimeNode->second);
            }
            const auto nestedRuntimeRoot = mapped.find(nestedRoot.id);
            if (nestedRuntimeRoot == mapped.end()) {
                diagnostics.push_back(ApplicationError("PXUISTUDIO2114", "Nested UI component root is missing from the Runtime projection", scenePath, runtimeRootId));
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
                diagnostics.push_back(ApplicationError("PXUISTUDIO2126", "UI component sourcePath does not match the UUID-authoritative source projection", scenePath, runtimeNode->second->id));
                pathsValid = false;
            }
        }
        if (pathsValid) applyInstance(*component, instance, mapped, root.id, 1);
    }
    if (!diagnostics.empty()) return Result<sdk::UiDocument>::Failure(std::move(diagnostics));
    return Result<sdk::UiDocument>::Success(std::move(document));
}

const Variant* ItemField(const Variant& item, std::string_view path) {
    const Variant* value = &item;
    while (!path.empty()) {
        const auto dot = path.find('.');
        const auto field = path.substr(0, dot);
        const auto* object = value->AsObject();
        if (!object) return nullptr;
        const auto found = object->find(std::string(field));
        if (found == object->end()) return nullptr;
        value = &found->second;
        path = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
    }
    return value;
}

std::optional<Rect> ItemPlacement(const Variant& item, std::string_view path) {
    const auto* value = ItemField(item, path);
    const auto* object = value ? value->AsObject() : nullptr;
    if (!object || object->size() != 4) return std::nullopt;
    const auto number = [&](const char* field) -> std::optional<float> {
        const auto found = object->find(field);
        if (found == object->end()) return std::nullopt;
        double result = 0.0;
        if (const auto* real = found->second.TryGet<double>()) result = *real;
        else if (const auto* integer = found->second.TryGet<std::int64_t>())
            result = static_cast<double>(*integer);
        else return std::nullopt;
        if (!std::isfinite(result) || std::abs(result) > 1'000'000.0)
            return std::nullopt;
        return static_cast<float>(result);
    };
    const auto x = number("x"), y = number("y"),
               width = number("width"), height = number("height");
    if (!x || !y || !width || !height || *width <= 0.0f || *height <= 0.0f)
        return std::nullopt;
    return Rect{*x, *y, *width, *height};
}

Status ConfigureDescriptorControls(scene::Node& node, const UiMotionLoader& loader) {
    if (auto* motion = dynamic_cast<MotionRect*>(&node)) {
        if (const auto status = motion->SetSourceLoader(loader); !status) return status;
    }
    if (auto* texture = dynamic_cast<TextureRect*>(&node)) {
        if (const auto status = texture->SetSourceLoader(loader); !status) return status;
    }
    for (const auto& child : node.Children()) {
        if (const auto status = ConfigureDescriptorControls(*child, loader); !status) return status;
    }
    return Status::Ok();
}

class AuthoredItemSource final : public IVirtualItemSource,
                                 public std::enable_shared_from_this<AuthoredItemSource> {
public:
    AuthoredItemSource(sdk::UiDocument document, sdk::UiItemSource descriptor,
                       IViewModel& model, VirtualizedView& view,
                       const FormatterRegistry& formatters,
                       UiAssetResolver assets, UiMotionLoader motionLoader,
                       UiActionSink actions,
                       std::string sourcePath)
        : m_document(std::move(document)), m_descriptor(std::move(descriptor)),
          m_model(model), m_view(view), m_formatters(formatters),
          m_assets(std::move(assets)), m_motionLoader(std::move(motionLoader)),
          m_actions(std::move(actions)),
          m_sourcePath(std::move(sourcePath)) {}

    ~AuthoredItemSource() override {
        if (m_subscription) m_model.Unsubscribe(m_subscription);
    }

    Status Initialize() {
        auto prototype = BuildUiRuntimeTree(m_document, m_assets);
        if (!prototype.Valid() || !prototype.unresolvedAssetIds.empty()) {
            std::vector<diag::Diagnostic> diagnostics;
            for (const auto& issue : prototype.diagnostics)
                diagnostics.push_back(ApplicationError(issue.code, issue.message,
                                                        m_sourcePath, issue.nodeId));
            for (const auto& asset : prototype.unresolvedAssetIds)
                diagnostics.push_back(ApplicationError("PXUISTUDIO2004",
                    "UI item template asset could not be resolved: " + asset,
                    m_sourcePath));
            if (diagnostics.empty()) diagnostics.push_back(ApplicationError(
                "PXUISTUDIO2140", "UI item template has no Runtime root", m_sourcePath));
            return Status::Fail(std::move(diagnostics));
        }
        if (const auto status = ConfigureDescriptorControls(*prototype.root, m_motionLoader); !status)
            return status;
        CollectPaths(*prototype.root, {});
        for (const auto& node : m_document.nodes) {
            const auto path = m_nodePaths.find(node.id);
            if (path == m_nodePaths.end()) return Status::Fail(ApplicationError(
                "PXUISTUDIO2141", "UI item template control is missing", m_sourcePath, node.id));
            auto* control = ControlAt(*prototype.root, path->second);
            for (const auto& [propertyName, binding] : node.bindings) {
                if (!binding.path.starts_with("item.") || binding.path.size() <= 5)
                    return Status::Fail(ApplicationError("PXUISTUDIO2142",
                        "UI item template binding must use item.<field>",
                        m_sourcePath, node.id, "bindings." + propertyName));
                const auto* property = control ? TypeRegistry::Global().FindProperty(
                    std::string(control->TypeName()), propertyName) : nullptr;
                if (!property || !property->set || !property->bindable)
                    return Status::Fail(ApplicationError("PXUISTUDIO2108",
                        "UI item template target is not bindable: " + propertyName,
                        m_sourcePath, node.id, "bindings." + propertyName));
                if (!binding.formatter.empty() && !m_formatters.Find(binding.formatter))
                    return Status::Fail(ApplicationError("PXUISTUDIO2143",
                        "Unknown UI item formatter: " + binding.formatter,
                        m_sourcePath, node.id, "bindings." + propertyName));
                m_bindings.push_back({path->second, propertyName,
                                      binding.path.substr(5), binding.formatter,
                                      property->type, node.id});
            }
        }
        auto initial = m_model.Read(m_descriptor.path);
        if (!initial) return Status::Fail(initial.Diagnostics());
        if (const auto status = Refresh(initial.Value()); !status) return status;
        m_subscription = m_model.Subscribe(m_descriptor.path,
            [weak = weak_from_this()](const Variant& value) {
                if (auto source = weak.lock()) {
                    const auto status = source->Refresh(value);
                    if (!status) {
                        for (const auto& diagnostic : status.Diagnostics()) diag::Emit(diagnostic);
                    } else source->m_view.InvalidateLayout();
                }
            });
        return Status::Ok();
    }

    std::size_t Count() const override { return m_items.size(); }
    std::string StableKey(const std::size_t index) const override {
        return index < m_keys.size() ? m_keys[index] : std::string{};
    }
    std::uint64_t Revision() const override { return m_revision; }
    std::optional<Rect> Placement(const std::size_t index) const override {
        return index < m_placements.size() ? std::optional<Rect>(m_placements[index]) : std::nullopt;
    }

    void Unbind(std::string_view, Control& item) override {
        if (const auto found = m_contexts.find(&item); found != m_contexts.end()) {
            found->second->index = static_cast<std::size_t>(-1);
            found->second->key.clear();
        }
    }

    Status Bind(const std::size_t index, Control& item) override {
        const auto context = m_contexts.find(&item);
        if (context == m_contexts.end() || index >= m_items.size())
            return Status::Fail(ApplicationError("PXUISTUDIO2144",
                "UI item template bind index or control is invalid", m_sourcePath));
        context->second->index = index;
        context->second->key = m_keys[index];
        for (const auto& binding : m_bindings) {
            auto* target = ControlAt(item, binding.nodePath);
            const auto* property = target ? TypeRegistry::Global().FindProperty(
                std::string(target->TypeName()), binding.property) : nullptr;
            const Variant* value = ItemField(m_items[index], binding.itemField);
            if (!property || !property->set || !value)
                return Status::Fail(ApplicationError("PXUISTUDIO2145",
                    "UI item field or target is missing: " + binding.itemField,
                    m_sourcePath, binding.nodeId, "bindings." + binding.property));
            const Variant* output = value;
            Variant formatted;
            if (!binding.formatter.empty()) {
                const auto* formatter = m_formatters.Find(binding.formatter);
                if (!formatter || formatter->input != value->Type())
                    return Status::Fail(ApplicationError("PXUISTUDIO2146",
                        "UI item formatter input type does not match: " + binding.formatter,
                        m_sourcePath, binding.nodeId, "bindings." + binding.property));
                auto result = formatter->format(*value);
                if (!result) return Status::Fail(result.Diagnostics());
                formatted = result.TakeValue();
                output = &formatted;
            }
            if (property->type != output->Type())
                return Status::Fail(ApplicationError("PXUISTUDIO2147",
                    "UI item binding output type does not match target: " + binding.property,
                    m_sourcePath, binding.nodeId, "bindings." + binding.property));
            if (const auto status = property->set(*target, *output); !status) return status;
        }
        return Status::Ok();
    }

    std::unique_ptr<Control> MakeItem() {
        auto context = std::make_shared<ItemContext>();
        auto tree = BuildUiRuntimeTree(m_document, m_assets,
            [weak = weak_from_this(), context](const sdk::UiAction& action,
                                                const std::string_view signal,
                                                const std::string_view nodeId) {
                if (auto source = weak.lock()) source->RunAction(*context, action, signal, nodeId);
            });
        if (!tree.Valid() || !tree.unresolvedAssetIds.empty()) {
            for (const auto& issue : tree.diagnostics)
                diag::Emit(ApplicationError(issue.code, issue.message,
                                             m_sourcePath, issue.nodeId));
            return nullptr;
        }
        if (const auto status = ConfigureDescriptorControls(*tree.root, m_motionLoader); !status) {
            for (const auto& diagnostic : status.Diagnostics()) diag::Emit(diagnostic);
            return nullptr;
        }
        context->root = tree.root.get();
        m_contexts.emplace(context->root, std::move(context));
        return std::move(tree.root);
    }

private:
    struct ItemContext {
        Control* root = nullptr;
        std::size_t index = 0;
        std::string key;
    };
    struct TemplateBinding {
        std::vector<std::size_t> nodePath;
        std::string property;
        std::string itemField;
        std::string formatter;
        VariantType targetType = VariantType::Null;
        std::string nodeId;
    };

    static Control* ControlAt(Control& root, const std::vector<std::size_t>& path) {
        scene::Node* node = &root;
        for (const auto step : path) {
            node = node->Child(step);
            if (!node) return nullptr;
        }
        return dynamic_cast<Control*>(node);
    }
    void CollectPaths(scene::Node& node, const std::vector<std::size_t>& path) {
        m_nodePaths.emplace(node.Id().ToString(), path);
        for (std::size_t index = 0; index < node.Children().size(); ++index) {
            auto childPath = path;
            childPath.push_back(index);
            CollectPaths(*node.Child(index), childPath);
        }
    }
    Status Refresh(const Variant& value) {
        const auto* array = value.AsArray();
        if (!array) return Status::Fail(ApplicationError("PXUISTUDIO2148",
            "UI itemSource path must contain an array: " + m_descriptor.path,
            m_sourcePath));
        std::vector<Variant> items;
        std::vector<Variant> rawKeys;
        std::vector<std::string> keys;
        std::vector<Rect> placements;
        std::unordered_set<std::string> unique;
        items.reserve(array->size()); rawKeys.reserve(array->size()); keys.reserve(array->size());
        placements.reserve(array->size());
        for (const auto& item : *array) {
            const auto* raw = ItemField(item, m_descriptor.key);
            if (!raw) return Status::Fail(ApplicationError("PXUISTUDIO2149",
                "UI item is missing stable key field: " + m_descriptor.key,
                m_sourcePath));
            std::string key;
            if (const auto* text = raw->TryGet<std::string>()) key = "s:" + *text;
            else if (const auto* integer = raw->TryGet<std::int64_t>())
                key = "i:" + std::to_string(*integer);
            else return Status::Fail(ApplicationError("PXUISTUDIO2150",
                "UI item stable key must be a string or integer", m_sourcePath));
            if (key.size() <= 2 || !unique.insert(key).second)
                return Status::Fail(ApplicationError("PXUISTUDIO2151",
                    "UI item stable keys must be nonempty and unique", m_sourcePath));
            if (!m_descriptor.placement.empty()) {
                const auto placement = ItemPlacement(item, m_descriptor.placement);
                if (!placement) return Status::Fail(ApplicationError("PXUISTUDIO2157",
                    "UI item placement must contain finite x/y and positive width/height: " +
                        m_descriptor.placement,
                    m_sourcePath, {}, "itemSource.placement"));
                placements.push_back(*placement);
            }
            for (const auto& binding : m_bindings) {
                const auto* field = ItemField(item, binding.itemField);
                if (!field) return Status::Fail(ApplicationError("PXUISTUDIO2145",
                    "UI item is missing a bound field: " + binding.itemField,
                    m_sourcePath, binding.nodeId, "bindings." + binding.property));
                VariantType outputType = field->Type();
                if (!binding.formatter.empty()) {
                    const auto* formatter = m_formatters.Find(binding.formatter);
                    if (!formatter || formatter->input != outputType)
                        return Status::Fail(ApplicationError("PXUISTUDIO2146",
                            "UI item formatter input type does not match: " + binding.formatter,
                            m_sourcePath, binding.nodeId, "bindings." + binding.property));
                    auto formatted = formatter->format(*field);
                    if (!formatted) return Status::Fail(formatted.Diagnostics());
                    outputType = formatted.Value().Type();
                }
                if (outputType != binding.targetType)
                    return Status::Fail(ApplicationError("PXUISTUDIO2147",
                        "UI item binding type does not match target: " + binding.property,
                        m_sourcePath, binding.nodeId, "bindings." + binding.property));
            }
            items.push_back(item.Clone()); rawKeys.push_back(raw->Clone());
            keys.push_back(std::move(key));
        }
        m_items = std::move(items);
        m_rawKeys = std::move(rawKeys);
        m_keys = std::move(keys);
        m_placements = std::move(placements);
        ++m_revision;
        return Status::Ok();
    }
    void RunAction(const ItemContext& context, const sdk::UiAction& source,
                   const std::string_view signal, const std::string_view nodeId) {
        if (!context.root || context.index >= m_items.size() ||
            m_keys[context.index] != context.key) return;
        sdk::UiAction action = source;
        for (auto& [name, value] : action.arguments) {
            (void)name;
            const auto* placeholder = std::get_if<std::string>(&value);
            if (!placeholder) continue;
            if (*placeholder == "$index") value = static_cast<std::int64_t>(context.index);
            else if (*placeholder == "$key") {
                if (const auto* text = m_rawKeys[context.index].TryGet<std::string>()) value = *text;
                else if (const auto* integer = m_rawKeys[context.index].TryGet<std::int64_t>())
                    value = *integer;
            }
        }
        const auto path = m_nodePaths.find(std::string(nodeId));
        auto* target = path == m_nodePaths.end() ? nullptr :
            ControlAt(*context.root, path->second);
        if (target && m_actions) m_actions(action, signal, target->Id().ToString());
    }

    sdk::UiDocument m_document;
    sdk::UiItemSource m_descriptor;
    IViewModel& m_model;
    VirtualizedView& m_view;
    const FormatterRegistry& m_formatters;
    UiAssetResolver m_assets;
    UiMotionLoader m_motionLoader;
    UiActionSink m_actions;
    std::string m_sourcePath;
    std::uint64_t m_subscription = 0;
    std::uint64_t m_revision = 0;
    std::vector<Variant> m_items;
    std::vector<Variant> m_rawKeys;
    std::vector<std::string> m_keys;
    std::vector<Rect> m_placements;
    std::vector<TemplateBinding> m_bindings;
    std::unordered_map<std::string, std::vector<std::size_t>> m_nodePaths;
    std::unordered_map<Control*, std::shared_ptr<ItemContext>> m_contexts;
};

}  // namespace

Result<std::vector<std::string>> CollectUiImageDependencies(
    const sdk::UiDocument& document, UiAssetResolver resolveAsset,
    UiComponentLoader loadComponent, UiMotionLoader loadMotion,
    std::string sourcePath) {
    auto resolved = ResolveComponents(document, loadComponent, sourcePath);
    if (!resolved) return Result<std::vector<std::string>>::Failure(resolved.Diagnostics());
    std::vector<std::string> paths;
    std::unordered_set<std::string> seen;
    const auto add = [&](std::string path) {
        if (!path.empty() && seen.insert(path).second) paths.push_back(std::move(path));
    };
    for (const auto& node : resolved.Value().nodes) {
        std::string assetPath;
        if (node.assetId) {
            const auto asset = resolveAsset ? resolveAsset(*node.assetId) : std::nullopt;
            if (!asset) return Result<std::vector<std::string>>::Failure(
                ApplicationError("PXUISTUDIO2004", "UI image asset could not be resolved: " + *node.assetId,
                                 sourcePath, node.id, "assetId"));
            assetPath = *asset;
        }
        const auto path = [&](const std::string_view property) -> std::string {
            const auto found = node.runtimeProperties.find(std::string(property));
            if (found == node.runtimeProperties.end()) return {};
            const auto* value = std::get_if<std::string>(&found->second);
            return value ? *value : std::string{};
        };
        const std::string type = node.runtimeType.value_or(
            node.kind == sdk::UiNodeKind::Image ? "TextureRect" : "");
        if (type != "TextureRect") add(assetPath);
        if (type == "MotionRect") {
            const std::string descriptor = path("path");
            if (descriptor.empty()) continue;
            if (!loadMotion) return Result<std::vector<std::string>>::Failure(
                ApplicationError("PXUIMOTION1002", "MotionRect requires a motion source loader",
                                 sourcePath, node.id, "runtimeProperties.path"));
            const auto text = loadMotion(descriptor);
            if (!text) return Result<std::vector<std::string>>::Failure(
                ApplicationError("PXUIMOTION1001", "Motion descriptor is missing: " + descriptor,
                                 sourcePath, node.id, "runtimeProperties.path"));
            auto motion = sdk::ParseMotion(*text, descriptor);
            if (!motion.Valid()) {
                std::vector<diag::Diagnostic> diagnostics;
                for (const auto& issue : motion.diagnostics)
                    diagnostics.push_back(ApplicationError(issue.code, issue.message,
                        descriptor, node.id, issue.path));
                return Result<std::vector<std::string>>::Failure(std::move(diagnostics));
            }
            for (auto& image : sdk::MotionDependencies(*motion.clip)) add(std::move(image));
        } else if (type == "SpriteText") {
            const auto found = node.runtimeProperties.find("glyphs");
            const auto* array = found == node.runtimeProperties.end()
                ? nullptr : std::get_if<sdk::UiArrayValue>(&found->second);
            if (!array) return Result<std::vector<std::string>>::Failure(
                ApplicationError("PXUISPRITE1001", "SpriteText requires a glyph array",
                                 sourcePath, node.id, "runtimeProperties.glyphs"));
            const Json glyphs = Json::parse(array->json, nullptr, false);
            if (!glyphs.is_array() || glyphs.empty() || glyphs.size() > 128)
                return Result<std::vector<std::string>>::Failure(
                ApplicationError("PXUISPRITE1001", "SpriteText glyph array is invalid",
                                 sourcePath, node.id, "runtimeProperties.glyphs"));
            for (const auto& glyph : glyphs) {
                if (!glyph.is_object() || !glyph.contains("path") ||
                    !glyph["path"].is_string())
                    return Result<std::vector<std::string>>::Failure(
                        ApplicationError("PXUISPRITE1001", "SpriteText glyph path is invalid",
                                         sourcePath, node.id, "runtimeProperties.glyphs"));
                add(glyph["path"].get<std::string>());
            }
        } else if (type == "TextureRect" || type == "NinePatchRect") {
            std::string image = path("path");
            if (type == "TextureRect" && image.empty()) {
                const auto found = node.runtimeProperties.find("texture");
                const auto* reference = found == node.runtimeProperties.end()
                    ? nullptr : std::get_if<sdk::UiResourceValue>(&found->second);
                if (reference && !reference->value.empty()) {
                    if (const auto id = Uuid::Parse(reference->value)) {
                        const auto resolvedAsset = resolveAsset
                            ? resolveAsset(id->ToString()) : std::nullopt;
                        if (!resolvedAsset) return Result<std::vector<std::string>>::Failure(
                            ApplicationError("PXUISTUDIO2004", "UI image asset could not be resolved: " + reference->value,
                                             sourcePath, node.id, "runtimeProperties.texture"));
                        image = *resolvedAsset;
                    } else {
                        image = reference->value;
                    }
                }
            }
            if (image.empty()) image = assetPath;
            if (type == "TextureRect" && image.ends_with(".pxportrait")) {
                if (!loadMotion) return Result<std::vector<std::string>>::Failure(
                    ApplicationError("PXUIPORTRAIT1003", "TextureRect requires a portrait source loader",
                                     sourcePath, node.id, "runtimeProperties.path"));
                const auto text = loadMotion(image);
                if (!text) return Result<std::vector<std::string>>::Failure(
                    ApplicationError("PXUIPORTRAIT1001", "Portrait descriptor is missing: " + image,
                                     sourcePath, node.id, "runtimeProperties.path"));
                const auto portrait = sdk::ParseLayeredPortrait(*text);
                if (!portrait) return Result<std::vector<std::string>>::Failure(
                    ApplicationError("PXUIPORTRAIT1002", "Portrait descriptor is invalid: " + image,
                                     sourcePath, node.id, "runtimeProperties.path"));
                for (const auto& layer : portrait->layers) add(layer.path);
            } else {
                add(image);
            }
        } else if (type == "TextureButton") {
            for (const auto property : {"normalPath", "hoverPath", "pressedPath", "disabledPath"})
                add(path(property));
        } else if (type == "Slider") {
            for (const auto property : {"railPath", "knobPath", "knobHoverPath", "knobPressedPath"})
                add(path(property));
        } else if (type == "VideoRect") {
            add(path("poster"));
        }
    }
    return Result<std::vector<std::string>>::Success(std::move(paths));
}

Result<UiApplicationSummary> UiApplication::ApplyText(const std::string_view json, UiApplicationOptions options) {
    const auto parsed = sdk::ParseUi(json);
    if (!parsed.Valid()) {
        return Result<UiApplicationSummary>::Failure(ParseDiagnostics(parsed, options.sourcePath));
    }
    return ApplyDocument(parsed.document, std::move(options));
}

Result<UiApplicationSummary> UiApplication::ApplyDocument(const sdk::UiDocument& document, UiApplicationOptions options) {
    if (const auto registered = RegisterBuiltinUITypes(); !registered) {
        return Result<UiApplicationSummary>::Failure(registered.Diagnostics());
    }

    auto resolved = ResolveComponents(document, options.loadComponent, options.sourcePath);
    if (!resolved) return Result<UiApplicationSummary>::Failure(resolved.Diagnostics());
    sdk::UiDocument resolvedDocument = std::move(resolved.Value());

    struct RepeaterTemplate {
        std::string ownerId;
        sdk::UiItemSource descriptor;
        sdk::UiDocument document;
    };
    std::unordered_map<std::string, const sdk::UiNode*> nodesById;
    for (const auto& node : resolvedDocument.nodes) nodesById.emplace(node.id, &node);
    std::vector<RepeaterTemplate> repeaters;
    std::unordered_set<std::string> templateNodeIds;
    for (const auto& node : resolvedDocument.nodes) {
        if (!node.itemSource) continue;
        if (!options.viewModel)
            return Result<UiApplicationSummary>::Failure(ApplicationError(
                "PXUISTUDIO2152", "UI itemSource requires a ViewModel",
                options.sourcePath, node.id, "itemSource"));
        std::vector<const sdk::UiNode*> children;
        for (const auto& candidate : resolvedDocument.nodes)
            if (candidate.parentId && *candidate.parentId == node.id)
                children.push_back(&candidate);
        if (children.size() != 1)
            return Result<UiApplicationSummary>::Failure(ApplicationError(
                "PXUISTUDIO2153", "UI itemSource requires exactly one item template",
                options.sourcePath, node.id, "itemSource"));
        RepeaterTemplate repeater;
        repeater.ownerId = node.id;
        repeater.descriptor = *node.itemSource;
        repeater.document = resolvedDocument;
        repeater.document.rootId = children.front()->id;
        repeater.document.behaviorGraph = {};
        repeater.document.behaviorTriggers.clear();
        repeater.document.visualStateGroups.clear();
        repeater.document.animations.reset();
        std::erase_if(repeater.document.nodes, [&](const sdk::UiNode& candidate) {
            const sdk::UiNode* cursor = &candidate;
            while (cursor && cursor->parentId) {
                if (*cursor->parentId == node.id) return false;
                const auto parent = nodesById.find(*cursor->parentId);
                cursor = parent == nodesById.end() ? nullptr : parent->second;
            }
            return true;
        });
        for (const auto& member : repeater.document.nodes) {
            if (member.itemSource)
                return Result<UiApplicationSummary>::Failure(ApplicationError(
                    "PXUISTUDIO2154", "Nested UI itemSource templates are not supported",
                    options.sourcePath, member.id, "itemSource"));
            if (!templateNodeIds.insert(member.id).second)
                return Result<UiApplicationSummary>::Failure(ApplicationError(
                    "PXUISTUDIO2155", "UI item templates overlap",
                    options.sourcePath, member.id));
        }
        repeaters.push_back(std::move(repeater));
    }
    sdk::UiDocument staticDocument = resolvedDocument;
    std::erase_if(staticDocument.nodes, [&](const sdk::UiNode& node) {
        return templateNodeIds.contains(node.id);
    });
    const auto templateTarget = [&](const std::string& nodeId,
                                    const std::string& property)
        -> std::optional<diag::Diagnostic> {
        if (!templateNodeIds.contains(nodeId)) return std::nullopt;
        return ApplicationError("PXUISTUDIO2157",
            "Behavior, Visual State, and Animation cannot target a repeated UI item; "
            "use item bindings or item Actions", options.sourcePath, nodeId, property);
    };
    for (const auto& trigger : resolvedDocument.behaviorTriggers)
        if (auto issue = templateTarget(trigger.nodeId, "behaviorTriggers"))
            return Result<UiApplicationSummary>::Failure(std::move(*issue));
    for (const auto& behavior : resolvedDocument.behaviorGraph.nodes)
        for (const auto& [name, value] : behavior.properties)
            if (const auto* reference = std::get_if<sdk::UiNodeReferenceValue>(&value))
                if (auto issue = templateTarget(reference->nodeId,
                                                "behaviorGraph." + name))
                    return Result<UiApplicationSummary>::Failure(std::move(*issue));
    for (const auto& behavior : resolvedDocument.behaviorGraph.nodes)
        for (const auto& [name, value] : behavior.arguments)
            if (const auto* reference = std::get_if<sdk::UiNodeReferenceValue>(&value))
                if (auto issue = templateTarget(reference->nodeId,
                                                "behaviorGraph.arguments." + name))
                    return Result<UiApplicationSummary>::Failure(std::move(*issue));
    for (const auto& group : resolvedDocument.visualStateGroups)
        for (const auto& state : group.states)
            for (const auto& overrideValue : state.overrides)
                if (auto issue = templateTarget(overrideValue.nodeId,
                                                "visualStateGroups"))
                    return Result<UiApplicationSummary>::Failure(std::move(*issue));
    if (resolvedDocument.animations)
        for (const auto& clip : resolvedDocument.animations->clips)
            for (const auto& track : clip.tracks)
                if (auto issue = templateTarget(track.nodeId, "animations"))
                    return Result<UiApplicationSummary>::Failure(std::move(*issue));

    UIContext* context = &m_context;
    const std::string sourcePath = options.sourcePath;
    const bool previewSafeMode = options.previewSafeMode;
    auto observer = std::move(options.observeAction);
    UiActionSink actionSink = [context, sourcePath, previewSafeMode, observer = std::move(observer)](const sdk::UiAction& action, const std::string_view signal, const std::string_view nodeId) {
            ActionInvocation invocation;
            invocation.action = action.id;
            invocation.context.preview = previewSafeMode;
            invocation.context.sourceScene = sourcePath;
            invocation.context.signal = "ui." + std::string(signal);
            if (const auto sourceNode = Uuid::Parse(nodeId)) invocation.context.sourceNode = *sourceNode;
            for (const auto& [name, source] : action.arguments) {
                auto value = ConvertUiActionValue(source);
                if (!value) {
                    const Status status = Status::Fail(ApplicationError(
                        "PXUISTUDIO2105",
                        "UI document Action argument cannot be represented by "
                        "the Runtime: " +
                            name,
                        sourcePath,
                        {},
                        "onClick.arguments." + name
                    ));
                    for (const auto& diagnostic : status.Diagnostics()) diag::Emit(diagnostic);
                    if (observer) observer(action, status);
                    return;
                }
                invocation.arguments.emplace(name, std::move(*value));
            }
            const Status status = context->Actions().Dispatch(std::move(invocation), { .previewSafeMode = previewSafeMode });
            if (observer) observer(action, status);
        };
    auto runtimeTree = BuildUiRuntimeTree(staticDocument, options.resolveAsset, actionSink);

    std::vector<diag::Diagnostic> diagnostics;
    for (const auto& source : runtimeTree.diagnostics) {
        diagnostics.push_back(ApplicationError(source.code, source.message, sourcePath, source.nodeId));
    }
    for (const auto& assetId : runtimeTree.unresolvedAssetIds) {
        diagnostics.push_back(ApplicationError("PXUISTUDIO2004", "UI document image asset could not be resolved: " + assetId, sourcePath, {}, "assetId"));
    }
    if (!runtimeTree.root && diagnostics.empty()) {
        diagnostics.push_back(ApplicationError("PXUISTUDIO2101", "UI document did not produce a Runtime root", sourcePath));
    }
    if (!diagnostics.empty()) return Result<UiApplicationSummary>::Failure(std::move(diagnostics));
    if (const auto status = ConfigureDescriptorControls(*runtimeTree.root, options.loadMotion); !status)
        return Result<UiApplicationSummary>::Failure(status.Diagnostics());

    std::vector<Binding> propertyBindings;
    for (const auto& node : staticDocument.nodes) {
        if (node.bindings.empty()) continue;
        if (!options.viewModel) {
            return Result<UiApplicationSummary>::Failure(ApplicationError("PXUISTUDIO2106", "UI document contains property bindings but no ViewModel was provided", sourcePath, node.id, "bindings"));
        }
        const auto nodeId = Uuid::Parse(node.id);
        auto* target = nodeId ? dynamic_cast<Control*>(runtimeTree.root->Find(*nodeId)) : nullptr;
        if (!target) {
            return Result<UiApplicationSummary>::Failure(ApplicationError("PXUISTUDIO2107", "UI document property binding target control is missing", sourcePath, node.id, "bindings"));
        }
        for (const auto& [targetName, descriptor] : node.bindings) {
            const auto* property = TypeRegistry::Global().FindProperty(std::string(target->TypeName()), targetName);
            if (!property || !property->set || !property->bindable) {
                return Result<UiApplicationSummary>::Failure(ApplicationError("PXUISTUDIO2108", "UI document binding target is not a bindable Runtime property: " + targetName, sourcePath, node.id, "bindings." + targetName));
            }
            BindingTarget bindingTarget{ property->type, node.name + "." + targetName, [target, property](const Variant& value) { return property->set(*target, value); } };
            auto binding = Binding::Create(*options.viewModel, descriptor.path, std::move(bindingTarget), m_context.Formatters(), descriptor.formatter);
            if (!binding) return Result<UiApplicationSummary>::Failure(binding.Diagnostics());
            propertyBindings.push_back(std::move(binding.Value()));
        }
    }

    for (auto& repeater : repeaters) {
        const auto ownerId = Uuid::Parse(repeater.ownerId);
        auto* view = ownerId ? dynamic_cast<VirtualizedView*>(runtimeTree.root->Find(*ownerId)) : nullptr;
        if (!view)
            return Result<UiApplicationSummary>::Failure(ApplicationError(
                "PXUISTUDIO2156", "UI itemSource target is not an item view",
                sourcePath, repeater.ownerId, "itemSource"));
        const bool placed = dynamic_cast<PlacedItemsView*>(view) != nullptr;
        if (placed != !repeater.descriptor.placement.empty())
            return Result<UiApplicationSummary>::Failure(ApplicationError(
                "PXUISTUDIO2158", "PlacedItemsView requires a placement field, other item views reject it",
                sourcePath, repeater.ownerId, "itemSource.placement"));
        auto source = std::make_shared<AuthoredItemSource>(
            std::move(repeater.document), std::move(repeater.descriptor),
            *options.viewModel, *view, m_context.Formatters(),
            options.resolveAsset, options.loadMotion, actionSink, sourcePath);
        if (const auto ready = source->Initialize(); !ready)
            return Result<UiApplicationSummary>::Failure(ready.Diagnostics());
        const auto installed = view->SetSource(source,
            [weak = std::weak_ptr<AuthoredItemSource>(source)] {
                if (auto live = weak.lock()) return live->MakeItem();
                return std::unique_ptr<Control>{};
            });
        if (!installed) return Result<UiApplicationSummary>::Failure(installed.Diagnostics());
    }

    if (runtimeTree.animations) {
        for (const auto& clip : runtimeTree.animations->clips) {
            for (const auto& track : clip.tracks) {
                auto* target = runtimeTree.root->Find(track.node);
                const auto* property = target ? TypeRegistry::Global().FindProperty(std::string(target->TypeName()), track.property) : nullptr;
                if (!target || !property || !property->get || !property->set ||
                    !property->animatable) {
                    return Result<UiApplicationSummary>::Failure(ApplicationError(
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

    for (const auto& group : runtimeTree.visualStateGroups) {
        for (const auto& state : group.states) {
            for (const auto& stateOverride : state.overrides) {
                auto* target = runtimeTree.root->Find(stateOverride.node);
                const auto* property = target
                    ? TypeRegistry::Global().FindProperty(
                          std::string(target->TypeName()),
                          stateOverride.property)
                    : nullptr;
                if (!target || !property || !property->get || !property->set ||
                    HasFlag(property->flags, PropertyFlags::ReadOnly) ||
                    (property->type != VariantType::Null &&
                     property->type != stateOverride.value.Type())) {
                    return Result<UiApplicationSummary>::Failure(
                        ApplicationError(
                            "PXUISTUDIO2134",
                            "Visual State target is not a matching writable Runtime property: " +
                                stateOverride.node.ToString() + "." +
                                stateOverride.property,
                            sourcePath, stateOverride.node.ToString(),
                            stateOverride.property));
                }
            }
        }
    }

    UiApplicationSummary summary{ .documentId = resolvedDocument.id,
                                        .revision = resolvedDocument.revision,
                                        .nodeCount = runtimeTree.nodeCount + templateNodeIds.size(),
                                        .actionBindingCount = runtimeTree.actionBindingCount,
                                        .behaviorNodeCount = runtimeTree.behaviorNodeCount,
                                        .behaviorTriggerCount = runtimeTree.behaviorTriggerCount,
                                        .animationClipCount = runtimeTree.animationClipCount,
                                        .animationTrackCount = runtimeTree.animationTrackCount,
                                        .visualStateGroupCount = runtimeTree.visualStateGroupCount,
                                        .visualStateCount = runtimeTree.visualStateCount,
                                        .propertyBindingCount = propertyBindings.size() };

    UIDocumentInstallation installation;
    installation.root=std::move(runtimeTree.root);
    installation.bindings=std::move(propertyBindings);
    installation.animations=std::move(runtimeTree.animations);
    installation.visualStateGroups=std::move(runtimeTree.visualStateGroups);
    installation.triggers=std::move(runtimeTree.behaviorTriggers);
    installation.behaviorGraph=std::move(runtimeTree.behaviorGraph);
    installation.sourceScene=sourcePath;
    installation.autoplayAnimations=true;
    installation.diagnosticOverlay=options.diagnosticOverlay;
    if(const auto installed=m_context.InstallDocument(std::move(installation));
       !installed)
        return Result<UiApplicationSummary>::Failure(installed.Diagnostics());
    return Result<UiApplicationSummary>::Success(std::move(summary));
}

Result<UiApplicationSummary> UiApplication::PatchDocumentProperties(const sdk::UiDocument& document, UiApplicationOptions options) {
    if (const auto registered = RegisterBuiltinUITypes(); !registered) {
        return Result<UiApplicationSummary>::Failure(registered.Diagnostics());
    }
    auto* installedRoot = m_context.Root();
    if (!installedRoot) {
        return Result<UiApplicationSummary>::Failure(ApplicationError("PXUISTUDIO2130", "UI document property patch requires an installed Runtime root", options.sourcePath));
    }

    auto resolved = ResolveComponents(document, options.loadComponent, options.sourcePath);
    if (!resolved) return Result<UiApplicationSummary>::Failure(resolved.Diagnostics());
    sdk::UiDocument resolvedDocument = std::move(resolved.Value());
    auto candidate = BuildUiRuntimeTree(resolvedDocument, std::move(options.resolveAsset));

    std::vector<diag::Diagnostic> diagnostics;
    for (const auto& source : candidate.diagnostics) {
        diagnostics.push_back(ApplicationError(source.code, source.message, options.sourcePath, source.nodeId));
    }
    for (const auto& assetId : candidate.unresolvedAssetIds) {
        diagnostics.push_back(ApplicationError("PXUISTUDIO2004", "UI document image asset could not be resolved: " + assetId, options.sourcePath, {}, "assetId"));
    }
    if (!candidate.root && diagnostics.empty()) {
        diagnostics.push_back(ApplicationError("PXUISTUDIO2131", "UI document property patch did not produce a candidate Runtime root", options.sourcePath));
    }
    if (!diagnostics.empty()) return Result<UiApplicationSummary>::Failure(std::move(diagnostics));
    if (const auto status = ConfigureDescriptorControls(*candidate.root, options.loadMotion); !status)
        return Result<UiApplicationSummary>::Failure(status.Diagnostics());

    struct MetadataPatch {
        Control* target = nullptr;
        std::string oldName;
        std::string newName;
        TokenRefValue oldStyle;
        TokenRefValue newStyle;
    };
    struct PropertyPatch {
        Control* target = nullptr;
        const PropertyInfo* property = nullptr;
        Variant oldValue;
        Variant newValue;
    };
    std::vector<MetadataPatch> metadataPatches;
    std::vector<PropertyPatch> propertyPatches;

    // Complete topology, type, range, resource, and setter validation against
    // the detached candidate before touching the installed tree.
    for (const auto& node : resolvedDocument.nodes) {
        const auto nodeId = Uuid::Parse(node.id);
        auto* installed = nodeId ? dynamic_cast<Control*>(installedRoot->Find(*nodeId)) : nullptr;
        const auto* source = nodeId ? dynamic_cast<const Control*>(candidate.root->Find(*nodeId)) : nullptr;
        if (!installed || !source || installed->TypeName() != source->TypeName()) {
            return Result<UiApplicationSummary>::Failure(ApplicationError("PXUISTUDIO2132", "UI document property patch topology does not match the installed Runtime tree", options.sourcePath, node.id));
        }
        metadataPatches.push_back({installed, installed->Name(), source->Name(),
                                   installed->StyleToken(), source->StyleToken()});
        for (const auto* property : TypeRegistry::Global().PropertiesForType(std::string(source->TypeName()))) {
            if (!property || !property->get || !property->set) continue;
            const Variant value = property->get(*source);
            if (property->get(*installed) == value) continue;
            if (property->type != VariantType::Null && value.Type() != property->type) {
                return Result<UiApplicationSummary>::Failure(ApplicationError(
                    "PXUISTUDIO2133", "UI document Runtime property patch type does not match metadata: " + property->name,
                    options.sourcePath, node.id, property->name));
            }
            if (property->editor.hasRange) {
                double numeric = 0.0;
                bool valid = false;
                if (const auto* number = value.TryGet<double>()) { numeric = *number; valid = true; }
                else if (const auto* integer = value.TryGet<std::int64_t>()) { numeric = static_cast<double>(*integer); valid = true; }
                if (!valid || numeric < property->editor.minimum || numeric > property->editor.maximum)
                    return Result<UiApplicationSummary>::Failure(ApplicationError(
                        "PXUISTUDIO2133", "UI document Runtime property patch is outside its declared range: " + property->name,
                        options.sourcePath, node.id, property->name));
            }
            propertyPatches.push_back({installed, property, property->get(*installed), value.Clone()});
        }
    }

    std::size_t committedProperties = 0;
    for (auto& patch : metadataPatches) {
        patch.target->SetName(patch.newName);
        patch.target->SetStyleToken(patch.newStyle);
    }
    for (; committedProperties < propertyPatches.size(); ++committedProperties) {
        auto& patch = propertyPatches[committedProperties];
        if (const auto status = patch.property->set(*patch.target, patch.newValue); !status) {
            for (std::size_t rollback = committedProperties; rollback > 0; --rollback) {
                auto& previous = propertyPatches[rollback - 1];
                (void)previous.property->set(*previous.target, previous.oldValue);
            }
            for (auto& metadata : metadataPatches) {
                metadata.target->SetName(metadata.oldName);
                metadata.target->SetStyleToken(metadata.oldStyle);
            }
            auto failure = ApplicationError("PXUISTUDIO2133", "UI document Runtime property patch failed transactionally: " + patch.property->name, options.sourcePath, patch.target->Id().ToString(), patch.property->name);
            failure.details = status.Diagnostics().empty() ? std::string{} : status.Diagnostics().front().message;
            return Result<UiApplicationSummary>::Failure(std::move(failure));
        }
    }

    std::size_t propertyBindingCount = 0;
    for (const auto& node : resolvedDocument.nodes) propertyBindingCount += node.bindings.size();
    m_context.SetDiagnosticOverlayEnabled(options.diagnosticOverlay);
    return Result<UiApplicationSummary>::Success(
        { .documentId = resolvedDocument.id,
          .revision = resolvedDocument.revision,
          .nodeCount = candidate.nodeCount,
          .actionBindingCount = candidate.actionBindingCount,
          .behaviorNodeCount = candidate.behaviorNodeCount,
          .behaviorTriggerCount = candidate.behaviorTriggerCount,
          .animationClipCount = candidate.animationClipCount,
          .animationTrackCount = candidate.animationTrackCount,
          .visualStateGroupCount = candidate.visualStateGroupCount,
          .visualStateCount = candidate.visualStateCount,
          .propertyBindingCount = propertyBindingCount }
    );
}

}  // namespace px::ui
