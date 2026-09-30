#include "Engine/SDK/UiJson.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <type_traits>
#include <vector>

namespace px::sdk {
namespace {

using Json = nlohmann::ordered_json;

const char* KindName(const UiNodeKind kind) {
    switch (kind) {
        case UiNodeKind::Control: return "control";
        case UiNodeKind::Label: return "label";
        case UiNodeKind::Button: return "button";
        case UiNodeKind::Image: return "image";
        case UiNodeKind::Stack: return "stack";
        case UiNodeKind::HBox: return "hbox";
        case UiNodeKind::VBox: return "vbox";
        case UiNodeKind::Grid: return "grid";
        case UiNodeKind::Group: return "group";
        case UiNodeKind::Leaf: return "leaf";
    }
    return "control";
}

Json Value(const UiValue& value) {
    return std::visit([](const auto& item) -> Json {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::monostate>) return nullptr;
        else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t> ||
                           std::is_same_v<T, double> || std::is_same_v<T, std::string>)
            return item;
        else if constexpr (std::is_same_v<T, UiVec2Value>)
            return Json{{"type", "vec2"}, {"x", item.x}, {"y", item.y}};
        else if constexpr (std::is_same_v<T, UiRectValue>)
            return Json{{"type", "rect"}, {"x", item.x}, {"y", item.y},
                        {"width", item.width}, {"height", item.height}};
        else if constexpr (std::is_same_v<T, UiColorValue>)
            return Json{{"type", "color"}, {"value", item.value}};
        else if constexpr (std::is_same_v<T, UiNodeReferenceValue>)
            return Json{{"type", "nodeReference"}, {"nodeId", item.nodeId}};
        else if constexpr (std::is_same_v<T, UiUuidValue>)
            return Json{{"type", "uuid"}, {"value", item.value}};
        else if constexpr (std::is_same_v<T, UiResourceValue>)
            return Json{{"type", "resource"}, {"value", item.value}};
        else if constexpr (std::is_same_v<T, UiTokenValue>)
            return Json{{"type", "token"}, {"value", item.value}};
        else return Json::parse(item.json);  // UiArrayValue / UiObjectValue
    }, value);
}

// Sorted keys keep the export stable across runs of the unordered maps.
template <typename Map, typename Encode>
Json SortedObject(const Map& map, Encode encode) {
    std::vector<std::string> keys;
    keys.reserve(map.size());
    for (const auto& [key, _] : map) keys.push_back(key);
    std::ranges::sort(keys);
    Json object = Json::object();
    for (const auto& key : keys) object[key] = encode(map.at(key));
    return object;
}

Json Node(const UiNode& node) {
    Json layout{
        {"mode", node.layout.mode == UiLayoutMode::Free ? "free" : "container"},
        {"x", node.layout.x}, {"y", node.layout.y},
        {"width", node.layout.width}, {"height", node.layout.height},
        {"anchorX", node.layout.anchorX}, {"anchorY", node.layout.anchorY},
        {"anchorRight", node.layout.anchorRight}, {"anchorBottom", node.layout.anchorBottom},
        {"pivotX", node.layout.pivotX}, {"pivotY", node.layout.pivotY},
        {"margin", node.layout.margin},
        {"alignment", node.layout.alignment}, {"sizeRule", node.layout.sizeRule}};
    Json appearance{{"backgroundColor", node.appearance.backgroundColor},
                    {"textColor", node.appearance.textColor},
                    {"opacity", node.appearance.opacity},
                    {"disabledOpacity", node.appearance.disabledOpacity}};
    if (node.appearance.styleToken) appearance["styleToken"] = *node.appearance.styleToken;
    if (node.appearance.hoverBackgroundColor)
        appearance["hoverBackgroundColor"] = *node.appearance.hoverBackgroundColor;
    if (node.appearance.focusColor) appearance["focusColor"] = *node.appearance.focusColor;

    Json out{{"id", node.id},
             {"parentId", node.parentId ? Json(*node.parentId) : Json(nullptr)},
             {"order", node.order}, {"kind", KindName(node.kind)}};
    if (node.runtimeType) out["runtimeType"] = *node.runtimeType;
    out["name"] = node.name;
    out["visible"] = node.visible;
    out["locked"] = node.locked;
    out["layout"] = std::move(layout);
    if (!node.text.empty()) out["text"] = node.text;
    if (node.assetId) out["assetId"] = *node.assetId;
    out["appearance"] = std::move(appearance);
    if (node.onClick)
        out["onClick"] = Json{{"id", node.onClick->id},
                              {"arguments", SortedObject(node.onClick->arguments, Value)}};
    if (!node.accessibilityLabel.empty()) out["accessibilityLabel"] = node.accessibilityLabel;
    if (!node.accessibilityRole.empty()) out["accessibilityRole"] = node.accessibilityRole;
    if (!node.accessibilityDescription.empty())
        out["accessibilityDescription"] = node.accessibilityDescription;
    if (node.accessibilityFocusOrder != 0)
        out["accessibilityFocusOrder"] = node.accessibilityFocusOrder;
    out["runtimeProperties"] = SortedObject(node.runtimeProperties, Value);
    out["bindings"] = SortedObject(node.bindings, [](const UiPropertyBinding& binding) {
        Json value{{"path", binding.path}};
        if (!binding.formatter.empty()) value["formatter"] = binding.formatter;
        return value;
    });
    if (node.itemSource) {
        Json source{{"path", node.itemSource->path}, {"key", node.itemSource->key}};
        if (!node.itemSource->placement.empty()) source["placement"] = node.itemSource->placement;
        out["itemSource"] = std::move(source);
    }
    return out;
}

}  // namespace

std::optional<std::string> WriteUiSceneJson(const UiDocument& document) {
    if (!document.behaviorGraph.nodes.empty() || !document.behaviorTriggers.empty() ||
        !document.visualStateGroups.empty() || document.animations.has_value())
        return std::nullopt;
    for (const auto& node : document.nodes)
        if (node.componentInstance || node.componentSlot) return std::nullopt;
    Json nodes = Json::array();
    for (const auto& node : document.nodes) nodes.push_back(Node(node));
    Json theme = Json::array();
    for (const auto& token : document.theme)
        theme.push_back(Json{{"id", token.id}, {"name", token.name}, {"value", token.value}});
    const Json out{{"format", "PrismatiXUIScene"}, {"schemaRevision", document.schemaRevision},
                   {"id", document.id}, {"revision", document.revision},
                   {"name", document.name}, {"width", document.width},
                   {"height", document.height}, {"rootId", document.rootId},
                   {"nodes", std::move(nodes)}, {"theme", std::move(theme)},
                   {"behaviorGraph", Json{{"nodes", Json::array()}, {"links", Json::array()},
                                          {"groups", Json::array()}}},
                   {"behaviorTriggers", Json::array()},
                   {"visualStateGroups", Json::array()}};
    return out.dump(2);
}

}  // namespace px::sdk
