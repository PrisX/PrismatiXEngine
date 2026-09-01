#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace px::test {

inline nlohmann::json CanonicalUiFixture(nlohmann::json document) {
    document["schemaRevision"] = 2;
    const bool component = document.value("format", std::string{}) ==
                           "PrismatiXUIComponent";
    if (!component) {
        if (!document.contains("behaviorGraph"))
            document["behaviorGraph"] = {{"nodes", nlohmann::json::array()},
                                          {"links", nlohmann::json::array()},
                                          {"groups", nlohmann::json::array()}};
        if (!document.contains("behaviorTriggers"))
            document["behaviorTriggers"] = nlohmann::json::array();
        if (!document.contains("visualStateGroups"))
            document["visualStateGroups"] = nlohmann::json::array();
    }
    for (auto& node : document["nodes"]) {
        auto& layout = node["layout"];
        if (!layout.contains("anchorRight"))
            layout["anchorRight"] = layout.value("anchorX", 0.0);
        if (!layout.contains("anchorBottom"))
            layout["anchorBottom"] = layout.value("anchorY", 0.0);
        if (const auto content = node.find("content"); content != node.end()) {
            if (content->contains("text") &&
                !(*content)["text"].get<std::string>().empty())
                node["text"] = (*content)["text"];
            if (content->contains("assetId") && !(*content)["assetId"].is_null())
                node["assetId"] = (*content)["assetId"];
            node.erase("content");
        }
        if (const auto interaction = node.find("interaction");
            interaction != node.end()) {
            if (interaction->contains("onClick") &&
                !(*interaction)["onClick"].is_null())
                node["onClick"] = (*interaction)["onClick"];
            node.erase("interaction");
        }
        if (const auto accessibility = node.find("accessibility");
            accessibility != node.end()) {
            if (accessibility->contains("label"))
                node["accessibilityLabel"] = (*accessibility)["label"];
            if (accessibility->contains("role"))
                node["accessibilityRole"] = (*accessibility)["role"];
            node.erase("accessibility");
        }
        if (!node.contains("runtimeProperties"))
            node["runtimeProperties"] = nlohmann::json::object();
        if (!node.contains("bindings")) node["bindings"] = nlohmann::json::object();
        if (auto instance = node.find("componentInstance");
            instance != node.end() && instance->is_object()) {
            if (!instance->contains("sourcePath"))
                (*instance)["sourcePath"] = nlohmann::json::array(
                    {(*instance)["componentId"], (*instance)["sourceNodeId"]});
            if (instance->contains("overrides")) {
                for (auto& value : (*instance)["overrides"]) {
                    if (!value.is_string()) continue;
                    std::string path = value.get<std::string>();
                    if (path == "content.text") path = "text";
                    else if (path == "content.assetId") path = "assetId";
                    else if (path == "interaction.onClick") path = "onClick";
                    else if (path == "accessibility.label") path = "accessibilityLabel";
                    else if (path == "accessibility.role") path = "accessibilityRole";
                    value = path;
                }
            }
        }
    }
    if (document.contains("componentInterface")) {
        for (auto& property : document["componentInterface"]["properties"]) {
            std::string path = property["property"].get<std::string>();
            if (path == "content.text") path = "text";
            else if (path == "content.assetId") path = "assetId";
            else if (path == "accessibility.label") path = "accessibilityLabel";
            else if (path == "accessibility.role") path = "accessibilityRole";
            property["property"] = path;
        }
    }
    return document;
}

inline std::string CanonicalUiFixtureText(const std::string_view text) {
    return CanonicalUiFixture(nlohmann::json::parse(text)).dump();
}

}  // namespace px::test
