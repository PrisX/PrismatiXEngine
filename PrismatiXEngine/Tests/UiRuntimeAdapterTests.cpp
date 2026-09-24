#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <SDL3/SDL_scancode.h>

#include "Engine/SDK/Ui.h"
#include "Engine/Platform/Input.h"
#include "Engine/UI/GalgameUI.h"
#include "Engine/UI/Actions/ActionCatalog.h"
#include "Engine/UI/UiAdapter.h"
#include "Engine/UI/UiApplication.h"
#include "Engine/UI/UIContext.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Engine/UI/VirtualizedView.h"
#include "Engine/UI/Widgets.h"

namespace {

void Check(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

bool HasDiagnostic(const px::Result<px::ui::UiApplicationSummary>& result, const std::string_view code) {
    return std::ranges::any_of(result.Diagnostics(), [&](const px::diag::Diagnostic& diagnostic) { return diagnostic.code == code; });
}

nlohmann::json CanonicalUi(nlohmann::json document) {
    document["schemaRevision"] = 2;
    const bool component = document.value("format", std::string{}) == "PrismatiXUIComponent";
    if (!component && !document.contains("behaviorGraph"))
        document["behaviorGraph"] = {{"nodes", nlohmann::json::array()},
                                      {"links", nlohmann::json::array()},
                                      {"groups", nlohmann::json::array()}};
    if (!component && !document.contains("behaviorTriggers")) document["behaviorTriggers"] = nlohmann::json::array();
    if (!component && !document.contains("visualStateGroups")) document["visualStateGroups"] = nlohmann::json::array();
    for (auto& node : document["nodes"]) {
        auto& layout = node["layout"];
        if (!layout.contains("anchorRight")) layout["anchorRight"] = layout.value("anchorX", 0.0);
        if (!layout.contains("anchorBottom")) layout["anchorBottom"] = layout.value("anchorY", 0.0);
        if (const auto content = node.find("content"); content != node.end()) {
            if (content->contains("text") && !(*content)["text"].get<std::string>().empty())
                node["text"] = (*content)["text"];
            if (content->contains("assetId") && !(*content)["assetId"].is_null())
                node["assetId"] = (*content)["assetId"];
            node.erase("content");
        }
        if (const auto interaction = node.find("interaction"); interaction != node.end()) {
            if (interaction->contains("onClick") && !(*interaction)["onClick"].is_null())
                node["onClick"] = (*interaction)["onClick"];
            node.erase("interaction");
        }
        if (const auto accessibility = node.find("accessibility"); accessibility != node.end()) {
            if (accessibility->contains("label")) node["accessibilityLabel"] = (*accessibility)["label"];
            if (accessibility->contains("role")) node["accessibilityRole"] = (*accessibility)["role"];
            node.erase("accessibility");
        }
        if (!node.contains("runtimeProperties")) node["runtimeProperties"] = nlohmann::json::object();
        if (!node.contains("bindings")) node["bindings"] = nlohmann::json::object();
    }
    if (document.contains("componentInterface")) {
        for (auto& property : document["componentInterface"]["properties"]) {
            auto path = property["property"].get<std::string>();
            if (path == "content.text") path = "text";
            else if (path == "content.assetId") path = "assetId";
            else if (path == "accessibility.label") path = "accessibilityLabel";
            else if (path == "accessibility.role") path = "accessibilityRole";
            property["property"] = path;
        }
    }
    return document;
}

std::string CanonicalUiText(const std::string_view text) {
    return CanonicalUi(nlohmann::json::parse(text)).dump();
}

std::string ComponentSource() {
    return CanonicalUiText(R"({
      "format":"PrismatiXUIComponent","schemaRevision":1,
      "id":"61616161-6161-4161-8161-616161616161","revision":3,
      "name":"ActionCard","width":420,"height":220,
      "rootId":"62626262-6262-4262-8262-626262626262",
      "nodes":[
        {"id":"62626262-6262-4262-8262-626262626262","parentId":null,"order":0,
         "kind":"group","name":"Card","visible":true,"locked":false,
         "layout":{"mode":"free","x":0,"y":0,"width":420,"height":220,"anchorX":0,"anchorY":0,"pivotX":0,"pivotY":0,"margin":0,"alignment":"start","sizeRule":"fixed"},
         "content":{"text":"","assetId":null},"appearance":{"backgroundColor":"#202030","textColor":"#FFFFFF","opacity":1,"styleToken":null,"hoverBackgroundColor":null,"focusColor":null,"disabledOpacity":0.5},
         "interaction":{"onClick":null},"accessibility":{"label":"Card","role":"group"}},
        {"id":"63636363-6363-4363-8363-636363636363","parentId":"62626262-6262-4262-8262-626262626262","order":0,
         "kind":"button","name":"Action","visible":true,"locked":false,
         "layout":{"mode":"free","x":20,"y":20,"width":180,"height":48,"anchorX":0,"anchorY":0,"pivotX":0,"pivotY":0,"margin":0,"alignment":"start","sizeRule":"fixed"},
         "content":{"text":"Default","assetId":null},"appearance":{"backgroundColor":"#F052A0","textColor":"#FFFFFF","opacity":1,"styleToken":null,"hoverBackgroundColor":null,"focusColor":null,"disabledOpacity":0.5},
         "interaction":{"onClick":null},"accessibility":{"label":"Action","role":"button"}},
        {"id":"64646464-6464-4464-8464-646464646464","parentId":"62626262-6262-4262-8262-626262626262","order":1,
         "kind":"vbox","name":"Content","visible":true,"locked":false,
         "layout":{"mode":"free","x":20,"y":88,"width":380,"height":112,"anchorX":0,"anchorY":0,"pivotX":0,"pivotY":0,"margin":0,"alignment":"start","sizeRule":"fixed"},
         "content":{"text":"","assetId":null},"appearance":{"backgroundColor":"#00000000","textColor":"#FFFFFF","opacity":1,"styleToken":null,"hoverBackgroundColor":null,"focusColor":null,"disabledOpacity":0.5},
         "interaction":{"onClick":null},"accessibility":{"label":"Content","role":"group"}}
      ],"theme":[],
      "componentInterface":{
        "properties":[{"id":"caption","displayName":"Caption","nodeId":"63636363-6363-4363-8363-636363636363","property":"content.text","valueType":"string","defaultValue":"Default"}],
        "signals":[{"id":"accepted","displayName":"Accepted","nodeId":"63636363-6363-4363-8363-636363636363","signal":"activated","arguments":[]}],
        "slots":[{"id":"content","displayName":"Content","nodeId":"64646464-6464-4464-8464-646464646464"}]
      }
    })");
}

std::string ComplexComponentSource() {
    using Json = nlohmann::json;
    Json component = Json::parse(ComponentSource());
    component["nodes"][0]["runtimeProperties"] = { { "stops", Json::array({ 0.25, 0.75 }) }, { "settings", { { "difficulty", "normal" }, { "flags", Json::array({ true }) } } } };
    auto& properties = component["componentInterface"]["properties"];
    properties.push_back(
        { { "id", "stops" }, { "displayName", "Stops" }, { "nodeId", component["nodes"][0]["id"] }, { "property", "runtimeProperties.stops" }, { "valueType", "array" }, { "defaultValue", component["nodes"][0]["runtimeProperties"]["stops"] } }
    );
    properties.push_back(
        { { "id", "settings" }, { "displayName", "Settings" }, { "nodeId", component["nodes"][0]["id"] }, { "property", "runtimeProperties.settings" }, { "valueType", "object" }, { "defaultValue", component["nodes"][0]["runtimeProperties"]["settings"] } }
    );
    return component.dump();
}

std::string ParameterizedComponentSource() {
    using Json = nlohmann::json;
    Json component = Json::parse(ComponentSource());
    component["schemaRevision"] = 2;
    auto& option = component["nodes"][1];
    option["kind"] = "leaf";
    option["runtimeType"] = "OptionButton";
    option["runtimeProperties"] = { { "options", Json::array({ "Prologue", "Finale" }) } };
    component["componentInterface"]["signals"][0]["signal"] = "itemSelected";
    component["componentInterface"]["signals"][0]["arguments"] = Json::array({ { { "id", "index" }, { "valueType", "integer" } }, { { "id", "text" }, { "valueType", "string" } } });
    return component.dump();
}

std::string ComponentScene(bool wrongSlotTarget, bool wrongPropertyType);

std::string ComplexComponentScene() {
    using Json = nlohmann::json;
    Json scene = Json::parse(ComponentScene(false, false));
    scene["nodes"][1]["componentInstance"]["publicProperties"]["stops"] = Json::array({ 0.1, 0.9 });
    scene["nodes"][1]["componentInstance"]["publicProperties"]["settings"] = { { "difficulty", "hard" }, { "flags", Json::array({ false, true }) } };
    return scene.dump();
}

std::string ParameterizedComponentScene() {
    using Json = nlohmann::json;
    Json scene = Json::parse(ComponentScene(false, false));
    scene["schemaRevision"] = 2;
    auto& option = scene["nodes"][2];
    option["kind"] = "leaf";
    option["runtimeType"] = "OptionButton";
    option["runtimeProperties"] = { { "options", Json::array({ "Prologue", "Finale" }) } };
    scene["nodes"][1]["componentInstance"]["publicSignals"]["accepted"] = { { "id", "choice.select" }, { "arguments", { { "index", 0 } } }, { "argumentBindings", { { "index", "index" } } } };
    return scene.dump();
}

std::string ComponentScene(const bool wrongSlotTarget = false, const bool wrongPropertyType = false) {
    const std::string slotParent = wrongSlotTarget ? "62626262-7272-4272-8272-626262626262" : "64646464-7474-4474-8474-646464646464";
    const std::string propertyValue = wrongPropertyType ? "42" : "\"Continue\"";
    return CanonicalUiText(std::string{
        R"({
      "format":"PrismatiXUIScene","schemaRevision":1,
      "id":"60606060-6060-4060-8060-606060606060","revision":7,
      "name":"ComponentScene","width":1280,"height":720,
      "rootId":"69696969-6969-4969-8969-696969696969",
      "nodes":[
        {"id":"69696969-6969-4969-8969-696969696969","parentId":null,"order":0,
         "kind":"control","name":"Root","visible":true,"locked":false,
         "layout":{"mode":"free","x":0,"y":0,"width":1280,"height":720,"anchorX":0,"anchorY":0,"pivotX":0,"pivotY":0,"margin":0,"alignment":"start","sizeRule":"fixed"},
         "content":{"text":"","assetId":null},"appearance":{"backgroundColor":"#101018","textColor":"#FFFFFF","opacity":1,"styleToken":null,"hoverBackgroundColor":null,"focusColor":null,"disabledOpacity":0.5},
         "interaction":{"onClick":null},"accessibility":{"label":"","role":"presentation"}},
        {"id":"62626262-7272-4272-8272-626262626262","parentId":"69696969-6969-4969-8969-696969696969","order":0,
         "kind":"group","name":"Card","visible":true,"locked":false,
         "layout":{"mode":"free","x":100,"y":100,"width":420,"height":220,"anchorX":0,"anchorY":0,"pivotX":0,"pivotY":0,"margin":0,"alignment":"start","sizeRule":"fixed"},
         "content":{"text":"","assetId":null},"appearance":{"backgroundColor":"#202030","textColor":"#FFFFFF","opacity":1,"styleToken":null,"hoverBackgroundColor":null,"focusColor":null,"disabledOpacity":0.5},
         "interaction":{"onClick":null},"accessibility":{"label":"Card","role":"group"},
         "componentInstance":{"componentId":"61616161-6161-4161-8161-616161616161","instanceRootId":"62626262-7272-4272-8272-626262626262","sourceNodeId":"62626262-6262-4262-8262-626262626262","sourcePath":["61616161-6161-4161-8161-616161616161","62626262-6262-4262-8262-626262626262"],"overrides":[],"publicProperties":{"caption":)"
    } + propertyValue +
           R"(},"publicSignals":{"accepted":{"id":"game.start","arguments":{}}}}},
        {"id":"63636363-7373-4373-8373-636363636363","parentId":"62626262-7272-4272-8272-626262626262","order":0,
         "kind":"button","name":"Action","visible":true,"locked":false,
         "layout":{"mode":"free","x":20,"y":20,"width":180,"height":48,"anchorX":0,"anchorY":0,"pivotX":0,"pivotY":0,"margin":0,"alignment":"start","sizeRule":"fixed"},
         "content":{"text":"Default","assetId":null},"appearance":{"backgroundColor":"#F052A0","textColor":"#FFFFFF","opacity":1,"styleToken":null,"hoverBackgroundColor":null,"focusColor":null,"disabledOpacity":0.5},
         "interaction":{"onClick":null},"accessibility":{"label":"Action","role":"button"},
         "componentInstance":{"componentId":"61616161-6161-4161-8161-616161616161","instanceRootId":"62626262-7272-4272-8272-626262626262","sourceNodeId":"63636363-6363-4363-8363-636363636363","sourcePath":["61616161-6161-4161-8161-616161616161","63636363-6363-4363-8363-636363636363"],"overrides":[]}},
        {"id":"64646464-7474-4474-8474-646464646464","parentId":"62626262-7272-4272-8272-626262626262","order":1,
         "kind":"vbox","name":"Content","visible":true,"locked":false,
         "layout":{"mode":"free","x":20,"y":88,"width":380,"height":112,"anchorX":0,"anchorY":0,"pivotX":0,"pivotY":0,"margin":0,"alignment":"start","sizeRule":"fixed"},
         "content":{"text":"","assetId":null},"appearance":{"backgroundColor":"#00000000","textColor":"#FFFFFF","opacity":1,"styleToken":null,"hoverBackgroundColor":null,"focusColor":null,"disabledOpacity":0.5},
         "interaction":{"onClick":null},"accessibility":{"label":"Content","role":"group"},
         "componentInstance":{"componentId":"61616161-6161-4161-8161-616161616161","instanceRootId":"62626262-7272-4272-8272-626262626262","sourceNodeId":"64646464-6464-4464-8464-646464646464","sourcePath":["61616161-6161-4161-8161-616161616161","64646464-6464-4464-8464-646464646464"],"overrides":[]}},
        {"id":"65656565-7575-4575-8575-656565656565","parentId":")" +
           slotParent + R"(","order":)" + (wrongSlotTarget ? "2" : "0") + R"(,
         "kind":"label","name":"LocalSlotContent","visible":true,"locked":false,
         "layout":{"mode":")" +
           (wrongSlotTarget ? "free" : "container") + R"(","x":0,"y":0,"width":120,"height":24,"anchorX":0,"anchorY":0,"pivotX":0,"pivotY":0,"margin":0,"alignment":"start","sizeRule":"fixed"},
         "content":{"text":"Local","assetId":null},"appearance":{"backgroundColor":"#00000000","textColor":"#FFFFFF","opacity":1,"styleToken":null,"hoverBackgroundColor":null,"focusColor":null,"disabledOpacity":0.5},
         "interaction":{"onClick":null},"accessibility":{"label":"Local","role":"text"},
         "componentSlot":{"instanceRootId":"62626262-7272-4272-8272-626262626262","slotId":"content"}}
      ],"theme":[]
    })");
}

std::string NestedComponentSource() {
    using Json = nlohmann::json;
    constexpr std::string_view outerId = "71717171-7171-4171-8171-717171717171";
    constexpr std::string_view outerRootId = "72727272-7272-4272-8272-727272727272";
    constexpr std::string_view nestedRootId = "73737373-7373-4373-8373-737373737373";
    const std::unordered_map<std::string, std::string> sourceIds{ { "62626262-6262-4262-8262-626262626262", std::string(nestedRootId) },
                                                                  { "63636363-6363-4363-8363-636363636363", "74747474-7474-4474-8474-747474747474" },
                                                                  { "64646464-6464-4464-8464-646464646464", "75757575-7575-4575-8575-757575757575" } };
    Json nested = Json::parse(ComponentSource());
    Json outer = nested;
    outer["id"] = outerId;
    outer["name"] = "NestedShell";
    outer["rootId"] = outerRootId;
    outer["componentInterface"] = { { "properties", Json::array() }, { "signals", Json::array() }, { "slots", Json::array() } };
    Json root = nested["nodes"][0];
    root["id"] = outerRootId;
    root["name"] = "NestedShell";
    outer["nodes"] = Json::array({ root });
    for (auto sourceNode : nested["nodes"]) {
        const std::string originalId = sourceNode["id"].get<std::string>();
        const bool isNestedRoot = originalId == nested["rootId"].get<std::string>();
        sourceNode["id"] = sourceIds.at(originalId);
        sourceNode["parentId"] = isNestedRoot ? Json(outerRootId) : Json(sourceIds.at(sourceNode["parentId"].get<std::string>()));
        sourceNode["componentInstance"] = { { "componentId", nested["id"] }, { "instanceRootId", nestedRootId }, { "sourceNodeId", originalId }, { "sourcePath", Json::array({ nested["id"], originalId }) }, { "overrides", Json::array() } };
        if (isNestedRoot) {
            sourceNode["componentInstance"]["publicProperties"] = { { "caption", "Nested continue" } };
            sourceNode["componentInstance"]["publicSignals"] = { { "accepted", { { "id", "game.start" }, { "arguments", Json::object() } } } };
        }
        outer["nodes"].push_back(std::move(sourceNode));
    }
    return outer.dump();
}

std::string NestedComponentScene() {
    using Json = nlohmann::json;
    constexpr std::string_view outerId = "71717171-7171-4171-8171-717171717171";
    constexpr std::string_view outerRootSourceId = "72727272-7272-4272-8272-727272727272";
    constexpr std::string_view nestedRootSourceId = "73737373-7373-4373-8373-737373737373";
    constexpr std::string_view runtimeRootId = "82828282-8282-4282-8282-828282828282";
    const std::unordered_map<std::string, std::string> runtimeIds{ { std::string(outerRootSourceId), std::string(runtimeRootId) },
                                                                   { std::string(nestedRootSourceId), "83838383-8383-4383-8383-838383838383" },
                                                                   { "74747474-7474-4474-8474-747474747474", "84848484-8484-4484-8484-848484848484" },
                                                                   { "75757575-7575-4575-8575-757575757575", "85858585-8585-4585-8585-858585858585" } };
    Json outer = Json::parse(NestedComponentSource());
    Json scene = outer;
    scene["format"] = "PrismatiXUIScene";
    scene["id"] = "80808080-8080-4080-8080-808080808080";
    scene["revision"] = 11;
    scene["name"] = "NestedComponentScene";
    scene["rootId"] = "81818181-8181-4181-8181-818181818181";
    scene.erase("componentInterface");
    Json viewport = outer["nodes"][0];
    viewport["id"] = scene["rootId"];
    viewport["parentId"] = nullptr;
    viewport["kind"] = "control";
    viewport["name"] = "Root";
    viewport["layout"]["width"] = 1280;
    viewport["layout"]["height"] = 720;
    viewport.erase("componentInstance");
    scene["nodes"] = Json::array({ viewport });
    for (auto sourceNode : outer["nodes"]) {
        const std::string sourceId = sourceNode["id"].get<std::string>();
        const bool isOuterRoot = sourceId == outerRootSourceId;
        const auto nestedOrigin = sourceNode.find("componentInstance");
        sourceNode["id"] = runtimeIds.at(sourceId);
        sourceNode["parentId"] = isOuterRoot ? scene["rootId"] : Json(runtimeIds.at(sourceNode["parentId"].get<std::string>()));
        Json sourcePath = Json::array({ outerId });
        if (nestedOrigin != sourceNode.end()) {
            sourcePath.push_back(nestedRootSourceId);
            sourcePath.push_back((*nestedOrigin)["componentId"]);
            sourcePath.push_back((*nestedOrigin)["sourceNodeId"]);
        }
        else {
            sourcePath.push_back(sourceId);
        }
        sourceNode["componentInstance"] = { { "componentId", outerId }, { "instanceRootId", runtimeRootId }, { "sourceNodeId", sourceId }, { "sourcePath", std::move(sourcePath) }, { "overrides", Json::array() } };
        scene["nodes"].push_back(std::move(sourceNode));
    }
    return scene.dump();
}

std::string NestedComponentSlotScene() {
    using Json = nlohmann::json;
    constexpr std::string_view outerComponentId = "71717171-7171-4171-8171-717171717171";
    constexpr std::string_view outerRootSourceId = "72727272-7272-4272-8272-727272727272";
    constexpr std::string_view nestedRootSourceId = "73737373-7373-4373-8373-737373737373";
    constexpr std::string_view slotInstanceRootId = "76767676-7676-4767-8767-767676767676";
    const std::unordered_map<std::string, std::string> runtimeIds{ { std::string(outerRootSourceId), std::string(slotInstanceRootId) },
                                                                   { std::string(nestedRootSourceId), "78787878-7878-4878-8878-787878787878" },
                                                                   { "74747474-7474-4474-8474-747474747474", "79797979-7979-4979-8979-797979797979" },
                                                                   { "75757575-7575-4575-8575-757575757575", "80808080-8080-4080-8080-808080808080" } };
    Json scene = Json::parse(ComponentScene());
    const Json component = Json::parse(NestedComponentSource());
    for (auto sourceNode : component["nodes"]) {
        const std::string sourceId = sourceNode["id"].get<std::string>();
        const bool isOuterRoot = sourceId == outerRootSourceId;
        const auto nestedOrigin = sourceNode.find("componentInstance");
        sourceNode["id"] = runtimeIds.at(sourceId);
        if (isOuterRoot) {
            sourceNode["order"] = 1;
            sourceNode["layout"]["mode"] = "container";
        }
        sourceNode["parentId"] = isOuterRoot ? Json("64646464-7474-4474-8474-646464646464") : Json(runtimeIds.at(sourceNode["parentId"].get<std::string>()));
        Json sourcePath = Json::array({ outerComponentId });
        if (nestedOrigin != sourceNode.end()) {
            sourcePath.push_back(nestedRootSourceId);
            sourcePath.push_back((*nestedOrigin)["componentId"]);
            sourcePath.push_back((*nestedOrigin)["sourceNodeId"]);
        }
        else {
            sourcePath.push_back(sourceId);
        }
        sourceNode["componentInstance"] = { { "componentId", outerComponentId }, { "instanceRootId", slotInstanceRootId }, { "sourceNodeId", sourceId }, { "sourcePath", std::move(sourcePath) }, { "overrides", Json::array() } };
        if (isOuterRoot) {
            sourceNode["componentSlot"] = { { "instanceRootId", "62626262-7272-4272-8272-626262626262" }, { "slotId", "content" } };
        }
        scene["nodes"].push_back(std::move(sourceNode));
    }
    return scene.dump();
}

std::string BudgetComponentId(const std::size_t index) {
    if (index == 0) return "61616161-6161-4161-8161-616161616161";
    std::ostringstream tail;
    tail << std::hex << std::setfill('0') << std::setw(12) << index;
    return "90909090-9090-4090-8090-" + tail.str();
}

std::string DependencyComponent(const std::string& id, const std::optional<std::string>& nextId) {
    using Json = nlohmann::json;
    Json component = Json::parse(ComponentSource());
    component["id"] = id;
    if (nextId) {
        const std::string sourceRootId = component["rootId"].get<std::string>();
        constexpr std::string_view nestedRootId = "92929292-9292-4292-8292-929292929292";
        Json nestedRoot = component["nodes"][0];
        nestedRoot["id"] = nestedRootId;
        nestedRoot["parentId"] = sourceRootId;
        nestedRoot["order"] = 2;
        nestedRoot["name"] = "Dependency";
        nestedRoot["componentInstance"] = { { "componentId", *nextId }, { "instanceRootId", nestedRootId }, { "sourceNodeId", sourceRootId }, { "sourcePath", Json::array({ *nextId, sourceRootId }) }, { "overrides", Json::array() } };
        component["nodes"].push_back(std::move(nestedRoot));
    }
    return component.dump();
}

}  // namespace

void TestAuthoredItemTemplate() {
    constexpr auto rootId = "a1a1a1a1-a1a1-41a1-81a1-a1a1a1a1a1a1";
    constexpr auto gridId = "a2a2a2a2-a2a2-42a2-82a2-a2a2a2a2a2a2";
    constexpr auto templateId = "a3a3a3a3-a3a3-43a3-83a3-a3a3a3a3a3a3";
    constexpr auto buttonId = "a4a4a4a4-a4a4-44a4-84a4-a4a4a4a4a4a4";
    constexpr auto imageId = "a5a5a5a5-a5a5-45a5-85a5-a5a5a5a5a5a5";
    px::sdk::UiDocument document;
    document.id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
    document.name = "Authored items";
    document.width = 300; document.height = 200;
    document.rootId = rootId;
    px::sdk::UiNode root;
    root.id = rootId; root.name = "Root"; root.kind = px::sdk::UiNodeKind::Group;
    root.layout.width = 300; root.layout.height = 200;
    px::sdk::UiNode grid;
    grid.id = gridId; grid.parentId = rootId; grid.name = "Slots";
    grid.kind = px::sdk::UiNodeKind::Grid; grid.runtimeType = "GridView";
    grid.layout.width = 300; grid.layout.height = 200;
    grid.runtimeProperties.emplace("columns", std::int64_t{2});
    grid.runtimeProperties.emplace("itemExtent", px::sdk::UiVec2Value{120, 60});
    grid.runtimeProperties.emplace("gap", px::sdk::UiVec2Value{10, 10});
    grid.itemSource = px::sdk::UiItemSource{"slots", "key"};
    px::sdk::UiNode item;
    item.id = templateId; item.parentId = gridId;
    item.name = "SlotTemplate"; item.kind = px::sdk::UiNodeKind::Group;
    item.layout.width = 120; item.layout.height = 60;
    px::sdk::UiNode button;
    button.id = buttonId; button.parentId = templateId;
    button.name = "SlotButton"; button.kind = px::sdk::UiNodeKind::Button;
    button.layout.width = 100; button.layout.height = 40;
    button.bindings.emplace("text", px::sdk::UiPropertyBinding{"item.title", {}});
    button.bindings.emplace("enabled", px::sdk::UiPropertyBinding{"item.enabled", {}});
    button.onClick = px::sdk::UiAction{"item.select", {{"index", std::string{"$index"}},
                                                      {"key", std::string{"$key"}}}};
    px::sdk::UiNode image;
    image.id = imageId; image.parentId = templateId;
    image.name = "BoundImage"; image.kind = px::sdk::UiNodeKind::Image;
    image.runtimeType = "TextureRect";
    image.layout.width = 100; image.layout.height = 40;
    image.bindings.emplace("path", px::sdk::UiPropertyBinding{"item.image", {}});
    document.nodes = {root, grid, item, button, image};

    const auto makeItem = [](std::string key, std::string title) {
        const std::string imagePath = "Assets/" + key + ".png";
        return px::Variant(px::VariantObject{{"key", px::Variant(std::move(key))},
                                             {"title", px::Variant(std::move(title))},
                                             {"image", px::Variant(imagePath)},
                                             {"enabled", px::Variant(true)}});
    };
    px::ui::ObservableViewModel model;
    Check(static_cast<bool>(model.Define("slots", px::Variant(px::VariantArray{
        makeItem("first", "First"), makeItem("second", "Second")}))),
        "item template test requires one typed collection");
    px::ui::UIContext context;
    std::int64_t selectedIndex = -1;
    std::string selectedKey;
    px::ui::ActionDescriptor selectDescriptor;
    selectDescriptor.id = "item.select";
    selectDescriptor.label = "Select item";
    selectDescriptor.displayName = "Select item";
    selectDescriptor.category = "Test";
    selectDescriptor.origin = px::ui::ActionOrigin::BuiltIn;
    selectDescriptor.sourceId = "AuthoredItemTest";
    selectDescriptor.providerId = "builtin";
    selectDescriptor.arguments = {
        {.name = "index", .type = px::VariantType::Integer, .required = true},
        {.name = "key", .type = px::VariantType::String, .required = true},
    };
    Check(context.Actions().Catalog().Register(std::move(selectDescriptor)).IsOk(),
          "item template Action descriptor must register");
    Check(static_cast<bool>(context.Commands().Register("item.select", [&](const px::Variant& arguments) {
        const auto* object = arguments.AsObject();
        if (!object) return px::Status::Fail(px::diag::Diagnostic{});
        const auto index = object->find("index"), key = object->find("key");
        if (index == object->end() || key == object->end() ||
            !index->second.TryGet<std::int64_t>() || !key->second.TryGet<std::string>())
            return px::Status::Fail(px::diag::Diagnostic{});
        selectedIndex = *index->second.TryGet<std::int64_t>();
        selectedKey = *key->second.TryGet<std::string>();
        return px::Status::Ok();
    })), "item template Action must register");
    px::ui::UiApplication application(context);
    const auto applied = application.ApplyDocument(document,
        {.sourcePath = "authored-items.pxui", .viewModel = &model});
    if (!applied) for (const auto& diagnostic : applied.Diagnostics())
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    Check(applied && applied.Value().nodeCount == 5,
          "one item template must install once as an authored definition");
    auto* view = dynamic_cast<px::ui::GridView*>(context.Root()->Find(*px::Uuid::Parse(gridId)));
    Check(view && view->HasSource() && view->Children().empty(),
          "item template must not become a static child of the GridView");
    (void)context.Root()->Measure({300, 200});
    context.Root()->Arrange({0, 0, 300, 200});
    Check(view->RealizedCount() == 2 && view->Children().size() == 2,
          "authored item source must realize one Control per visible data item");
    Check(context.MountedImageDependencies() ==
              std::vector<std::string>{"Assets/first.png", "Assets/second.png"},
          "mounted image dependencies resolve item.image on realized repeater children");
    auto* first = dynamic_cast<px::ui::Control*>(view->Child(0));
    auto* firstButton = first ? dynamic_cast<px::ui::Button*>(first->Child(0)) : nullptr;
    Check(firstButton && firstButton->Text() == "First",
          "item.* binding must set the Runtime property on each clone");
    const auto firstId = first->Id();
    firstButton->Activate();
    Check(selectedIndex == 0 && selectedKey == "first",
          "item Action placeholders must resolve to the current index and key");

    Check(static_cast<bool>(model.Write("slots", px::Variant(px::VariantArray{
        makeItem("second", "Second changed"), makeItem("first", "First changed")}))),
        "repeater collection must allow reorder without indexed subscriptions");
    (void)context.Root()->Measure({300, 200});
    context.Root()->Arrange({0, 0, 300, 200});
    auto* moved = dynamic_cast<px::ui::Control*>(view->Child(1));
    auto* movedButton = moved ? dynamic_cast<px::ui::Button*>(moved->Child(0)) : nullptr;
    Check(moved && moved->Id() == firstId && movedButton &&
              movedButton->Text() == "First changed",
          "stable key reorder must retain Control identity and refresh item bindings");
    movedButton->Activate();
    Check(selectedIndex == 1 && selectedKey == "first",
          "reordered item Actions must use the new index, not their original index");
    Check(static_cast<bool>(model.Write("slots", px::Variant(px::VariantArray{makeItem("first", "Only")}))),
          "repeater collection must allow shrinking");
    (void)context.Root()->Measure({300, 200});
    context.Root()->Arrange({0, 0, 300, 200});
    Check(view->RealizedCount() == 1 && view->Child(0)->Id() == firstId,
          "shrinking collection must recycle stale controls and retain surviving key");
    Check(context.MountedImageDependencies() ==
              std::vector<std::string>{"Assets/first.png"},
          "mounted image dependencies exclude virtualized items after collection shrink");
    Check(static_cast<bool>(model.Write("slots", px::Variant(px::VariantArray{
        makeItem("first", "Ambiguous"), makeItem("first", "Duplicate")}))),
          "ViewModel can receive an invalid application-level item snapshot");
    (void)context.Root()->Measure({300, 200});
    context.Root()->Arrange({0, 0, 300, 200});
    auto* retained = dynamic_cast<px::ui::Control*>(view->Child(0));
    auto* retainedButton = retained ? dynamic_cast<px::ui::Button*>(retained->Child(0)) : nullptr;
    Check(view->RealizedCount() == 1 && retainedButton &&
              retainedButton->Text() == "Only",
          "invalid duplicate keys must retain the previous visible item snapshot");
}

void TestAuthoredPlacedItemTemplate() {
    constexpr auto rootId = "c1c1c1c1-c1c1-41c1-81c1-c1c1c1c1c1c1";
    constexpr auto viewId = "c2c2c2c2-c2c2-42c2-82c2-c2c2c2c2c2c2";
    constexpr auto templateId = "c3c3c3c3-c3c3-43c3-83c3-c3c3c3c3c3c3";
    constexpr auto buttonId = "c4c4c4c4-c4c4-44c4-84c4-c4c4c4c4c4c4";
    px::sdk::UiDocument document;
    document.id = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
    document.name = "Placed settings";
    document.width = 400; document.height = 300; document.rootId = rootId;
    px::sdk::UiNode root;
    root.id = rootId; root.name = "Root"; root.kind = px::sdk::UiNodeKind::Group;
    root.layout.width = 400; root.layout.height = 300;
    px::sdk::UiNode view;
    view.id = viewId; view.parentId = rootId; view.name = "Switches";
    view.kind = px::sdk::UiNodeKind::Control; view.runtimeType = "PlacedItemsView";
    view.layout.x = 10; view.layout.y = 20;
    view.layout.width = 300; view.layout.height = 200;
    view.itemSource = px::sdk::UiItemSource{"rows", "key", "rect"};
    px::sdk::UiNode item;
    item.id = templateId; item.parentId = viewId; item.name = "SwitchTemplate";
    item.kind = px::sdk::UiNodeKind::Group;
    item.layout.width = 80; item.layout.height = 25;
    px::sdk::UiNode button;
    button.id = buttonId; button.parentId = templateId;
    button.name = "SwitchButton"; button.kind = px::sdk::UiNodeKind::Button;
    button.layout.width = 80; button.layout.height = 25;
    button.bindings.emplace("text", px::sdk::UiPropertyBinding{"item.title", {}});
    document.nodes = {root, view, item, button};

    const auto makeRow = [](std::string key, std::string title, std::int64_t x,
                            std::int64_t y, std::int64_t width = 80) {
        return px::Variant(px::VariantObject{
            {"key", px::Variant(std::move(key))},
            {"title", px::Variant(std::move(title))},
            {"rect", px::Variant(px::VariantObject{
                {"x", px::Variant(x)}, {"y", px::Variant(y)},
                {"width", px::Variant(width)}, {"height", px::Variant(std::int64_t{25})}})},
        });
    };
    px::ui::ObservableViewModel model;
    Check(static_cast<bool>(model.Define("rows", px::Variant(px::VariantArray{
        makeRow("first", "First", 20, 30), makeRow("second", "Second", 150, 90)}))),
        "placed template requires one typed collection");
    px::ui::UIContext context;
    px::ui::UiApplication application(context);
    const auto applied = application.ApplyDocument(document,
        {.sourcePath = "placed-items.pxui", .viewModel = &model});
    if (!applied) for (const auto& diagnostic : applied.Diagnostics())
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    Check(applied && applied.Value().nodeCount == 4,
          "placed template must install as one authored definition");
    auto* placed = dynamic_cast<px::ui::PlacedItemsView*>(
        context.Root()->Find(*px::Uuid::Parse(viewId)));
    Check(placed && placed->HasSource() && placed->Children().empty(),
          "placed item template must not become a static child");
    (void)context.Root()->Measure({400, 300});
    context.Root()->Arrange({0, 0, 400, 300});
    Check(placed->RealizedCount() == 2, "placed source must realize both item controls");
    auto* first = dynamic_cast<px::ui::Control*>(placed->Child(0));
    auto* firstButton = first ? dynamic_cast<px::ui::Button*>(first->Child(0)) : nullptr;
    Check(first && firstButton && firstButton->Text() == "First" &&
          first->LayoutRect().x == placed->LayoutRect().x + 20 &&
          first->LayoutRect().y == placed->LayoutRect().y + 30 &&
          first->LayoutRect().w == 80 && first->LayoutRect().h == 25,
          "placed item must use its data rectangle relative to the owner");
    const auto firstId = first->Id();

    Check(static_cast<bool>(model.Write("rows", px::Variant(px::VariantArray{
        makeRow("second", "Second moved", 30, 40),
        makeRow("first", "First moved", 170, 110, 90)}))),
        "placed source must allow data-driven reposition and reorder");
    (void)context.Root()->Measure({400, 300});
    context.Root()->Arrange({0, 0, 400, 300});
    auto* moved = dynamic_cast<px::ui::Control*>(placed->Child(1));
    auto* movedButton = moved ? dynamic_cast<px::ui::Button*>(moved->Child(0)) : nullptr;
    Check(moved && moved->Id() == firstId && movedButton &&
          movedButton->Text() == "First moved" &&
          moved->LayoutRect().x == placed->LayoutRect().x + 170 &&
          moved->LayoutRect().w == 90,
          "stable-key reorder must retain identity and update placement");

    Check(static_cast<bool>(model.Write("rows", px::Variant(px::VariantArray{
        makeRow("first", "Invalid", 20, 30, 0)}))),
        "ViewModel may receive a malformed application-level placement");
    (void)context.Root()->Measure({400, 300});
    context.Root()->Arrange({0, 0, 400, 300});
    auto* retained = dynamic_cast<px::ui::Control*>(placed->Child(1));
    auto* retainedButton = retained ? dynamic_cast<px::ui::Button*>(retained->Child(0)) : nullptr;
    Check(placed->RealizedCount() == 2 && retained && retained->Id() == firstId &&
          retainedButton && retainedButton->Text() == "First moved",
          "invalid placement must retain the previous visible snapshot");
}

void TestGalgameAuthoredSlots() {
    constexpr auto rootId = "b1b1b1b1-b1b1-41b1-81b1-b1b1b1b1b1b1";
    constexpr auto gridId = "b2b2b2b2-b2b2-42b2-82b2-b2b2b2b2b2b2";
    constexpr auto itemId = "b3b3b3b3-b3b3-43b3-83b3-b3b3b3b3b3b3";
    constexpr auto buttonId = "b4b4b4b4-b4b4-44b4-84b4-b4b4b4b4b4b4";
    const auto layout = [](const int width, const int height, const char* mode = "free") {
        return nlohmann::json{{"mode", mode}, {"x", 0}, {"y", 0},
            {"width", width}, {"height", height}, {"anchorX", 0}, {"anchorY", 0},
            {"anchorRight", 0}, {"anchorBottom", 0}, {"pivotX", 0}, {"pivotY", 0},
            {"margin", 0}, {"alignment", "start"}, {"sizeRule", "fixed"}};
    };
    const auto node = [&](const char* id, const nlohmann::json& parent,
                          const char* kind, const char* runtimeType,
                          const char* name, const int width, const int height,
                          const char* mode = "free") {
        return nlohmann::json{{"id", id}, {"parentId", parent}, {"order", 0},
            {"kind", kind}, {"runtimeType", runtimeType}, {"name", name},
            {"visible", true}, {"locked", false},
            {"layout", layout(width, height, mode)},
            {"runtimeProperties", nlohmann::json::object()},
            {"bindings", nlohmann::json::object()}};
    };
    auto root = node(rootId, nullptr, "group", "Control", "Root", 600, 400);
    auto grid = node(gridId, rootId, "grid", "GridView", "Slots", 600, 400);
    grid["itemSource"] = {{"path", "saveLoad.slots"}, {"key", "key"}};
    grid["runtimeProperties"] = {
        {"columns", 4}, {"itemExtent", {{"type", "vec2"}, {"x", 100}, {"y", 70}}},
        {"gap", {{"type", "vec2"}, {"x", 10}, {"y", 10}}}};
    auto item = node(itemId, gridId, "group", "Control", "SlotTemplate",
                     100, 70, "container");
    auto button = node(buttonId, itemId, "button", "Button", "SlotButton", 100, 70);
    button["bindings"] = {{"text", {{"path", "item.title"}}},
                          {"enabled", {{"path", "item.enabled"}}}};
    button["onClick"] = {{"id", "save.slot"},
                         {"arguments", {{"slot", "$index"}}}};
    auto document = CanonicalUi(nlohmann::json{
        {"format", "PrismatiXUIScene"}, {"schemaRevision", 2},
        {"id", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"}, {"revision", 1},
        {"name", "Authored Save"}, {"width", 600}, {"height", 400},
        {"rootId", rootId}, {"nodes", nlohmann::json::array({root, grid, item, button})},
        {"theme", nlohmann::json::array()}});
    px::ui::GalgameUI galgame;
    const auto registered = galgame.RegisterTemplate(px::ui::GalgameUI::Screen::Save,
        document.dump(), "authored-save.pxui");
    if (!registered) for (const auto& diagnostic : registered.Diagnostics())
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    Check(registered.IsOk(), "GalgameUI must accept an authored item template route");
    px::ui::GalgameItem slot;
    slot.key = "slot-0"; slot.title = "Saved scene";
    slot.command = "save.slot"; slot.argument = "0";
    const auto shown = galgame.ShowSaveLoad(true, {slot});
    if (!shown) for (const auto& diagnostic : shown.Diagnostics())
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    Check(shown.IsOk(), "ShowSaveLoad must retain an authored keyed GridView source");
    auto* view = dynamic_cast<px::ui::GridView*>(galgame.Root()->Find(*px::Uuid::Parse(gridId)));
    Check(view && view->HasSource(), "authored Save route must keep its ViewModel item source");
    (void)galgame.Root()->Measure({600, 400});
    galgame.Root()->Arrange({0, 0, 600, 400});
    Check(view->RealizedCount() == 12, "authored Save route must realize all 12 supplied slots");
    auto* first = dynamic_cast<px::ui::Control*>(view->Child(0));
    auto* firstButton = first ? dynamic_cast<px::ui::Button*>(first->Child(0)) : nullptr;
    Check(firstButton && firstButton->Text() == "Saved scene" && firstButton->Enabled(),
          "GalgameUI ViewModel must populate authored item bindings");
    std::string selected;
    galgame.SetActionSink([&](const px::ui::GalgameAction& action) {
        if (action.command == "save.slot") selected = action.argument;
    });
    firstButton->Activate();
    px::Input input;
    (void)galgame.Update(input, 600, 400);
    Check(selected == "0", "authored Save item must dispatch the selected slot index");

    auto placedDocument = document;
    placedDocument["nodes"][1]["kind"] = "control";
    placedDocument["nodes"][1]["runtimeType"] = "PlacedItemsView";
    placedDocument["nodes"][1]["runtimeProperties"] = nlohmann::json::object();
    placedDocument["nodes"][1]["itemSource"] =
        {{"path", "custom.rows"}, {"key", "key"}, {"placement", "rect"}};
    const auto placedRow = [](const char* key, const char* title, const int x) {
        return px::Variant(px::VariantObject{
            {"key", px::Variant(key)}, {"title", px::Variant(title)},
            {"enabled", px::Variant(true)},
            {"rect", px::Variant(px::VariantObject{
                {"x", px::Variant(x)}, {"y", px::Variant(25)},
                {"width", px::Variant(100)}, {"height", px::Variant(70)}})},
        });
    };
    px::ui::GalgameUI authoredData;
    Check(authoredData.DefineUiData("custom.rows", px::Variant(px::VariantArray{
        placedRow("one", "One", 20), placedRow("two", "Two", 180)})).IsOk(),
          "GalgameUI must accept initial author-defined UI data");
    Check(!authoredData.DefineUiData("custom.rows", px::Variant(px::VariantArray{})) &&
          !authoredData.DefineUiData("custom..invalid", px::Variant(1)),
          "GalgameUI must reject duplicate and invalid authored data paths");
    Check(authoredData.RegisterRouteTemplate("custom-placed", placedDocument.dump(),
          "custom-placed.pxui").IsOk(),
          "GalgameUI must accept a placed item route");
    const auto placedShown = authoredData.ShowRoute("custom-placed");
    if (!placedShown) for (const auto& diagnostic : placedShown.Diagnostics())
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    Check(placedShown.IsOk(), "author-defined data must be available before route mount");
    (void)authoredData.Root()->Measure({600, 400});
    authoredData.Root()->Arrange({0, 0, 600, 400});
    auto* placedView = dynamic_cast<px::ui::PlacedItemsView*>(
        authoredData.Root()->Find(*px::Uuid::Parse(gridId)));
    auto* placedFirst = placedView && placedView->Children().size() == 2
        ? dynamic_cast<px::ui::Control*>(placedView->Child(0)) : nullptr;
    auto* placedButton = placedFirst
        ? dynamic_cast<px::ui::Button*>(placedFirst->Child(0)) : nullptr;
    Check(placedView && placedView->RealizedCount() == 2 && placedFirst &&
          placedButton && placedButton->Text() == "One" &&
          placedFirst->LayoutRect().x == placedView->LayoutRect().x + 20,
          "GalgameUI must bind and place items from the author-defined data source");
}

void TestMountedImageDependencies() {
    px::ui::UIContext context;
    auto root = std::make_unique<px::ui::Control>("Root");
    auto image = std::make_unique<px::ui::TextureRect>("Assets/initial.png");
    auto* boundImage = image.get();
    Check(root->AddChild(std::move(image)).IsOk(), "mounted image fixture attaches");
    auto button = std::make_unique<px::ui::TextureButton>();
    button->SetNormalPath("Assets/initial.png");
    button->SetHoverPath("Assets/hover.png");
    Check(root->AddChild(std::move(button)).IsOk(), "mounted button fixture attaches");
    Check(context.SetRoot(std::move(root)).IsOk(), "mounted image fixture installs");
    const auto initial = context.MountedImageDependencies();
    Check(initial == std::vector<std::string>{"Assets/initial.png", "Assets/hover.png"},
          "mounted image dependencies include interaction states without duplicate paths");
    boundImage->SetPath("Assets/bound.png");
    const auto rebound = context.MountedImageDependencies();
    Check(rebound == std::vector<std::string>{"Assets/bound.png", "Assets/initial.png", "Assets/hover.png"},
          "mounted image dependencies observe runtime-bound path changes");
    std::vector<std::string> requested;
    context.SetImagePreloadSink([&requested](const std::string_view path) {
        requested.emplace_back(path);
    });
    px::Input neutral;
    (void)context.Update(neutral, 300, 200, 0.0f);
    Check(requested == rebound,
          "UI update requests the paths resolved after binding and layout");
    boundImage->SetPath("Assets/changed-again.png");
    requested.clear();
    (void)context.Update(neutral, 300, 200, 0.0f);
    Check(std::ranges::find(requested, "Assets/changed-again.png") != requested.end(),
          "UI update requests images changed after the route was mounted");
}

int main() {
    TestAuthoredItemTemplate();
    TestAuthoredPlacedItemTemplate();
    TestGalgameAuthoredSlots();
    TestMountedImageDependencies();
    Check(px::ui::RegisterBuiltinUITypes().IsOk(), "typed caption fixture requires builtin controls");
    {
        px::sdk::UiDocument bindingScene;
        bindingScene.id = "12000000-0000-4000-8000-000000000001";
        bindingScene.rootId = "12000000-0000-4000-8000-000000000002";
        px::sdk::UiNode root;
        root.id = bindingScene.rootId;
        root.kind = px::sdk::UiNodeKind::Group;
        root.runtimeType = "Control";
        root.layout.width = 800;
        root.layout.height = 600;
        px::sdk::UiNode keyButton;
        keyButton.id = "12000000-0000-4000-8000-000000000003";
        keyButton.parentId = root.id;
        keyButton.kind = px::sdk::UiNodeKind::Button;
        keyButton.runtimeType = "TextureButton";
        keyButton.text = "F2 / S";
        keyButton.layout.width = 247;
        keyButton.layout.height = 34;
        keyButton.runtimeProperties.emplace("bindingAction", "original-save");
        bindingScene.nodes = {root, keyButton};
        auto tree = px::ui::BuildUiRuntimeTree(bindingScene);
        Check(tree.Valid(), "authored TextureButton bindingAction adapts to Runtime");
        px::Input keys;
        Check(keys.RegisterAction("original-save") &&
                  keys.BindKey("original-save", SDL_SCANCODE_F2) &&
                  keys.BindKey("original-save", SDL_SCANCODE_S),
              "authored binding fixture registers its original shortcut");
        px::ui::UIContext context;
        context.SetInputBindingService(&keys);
        Check(context.SetRoot(std::move(tree.root)).IsOk(),
              "authored binding button mounts with the input service");
        const auto* mounted = dynamic_cast<const px::ui::TextureButton*>(
            context.Root()->Find(*px::Uuid::Parse(keyButton.id)));
        Check(mounted && mounted->BindingAction() == "original-save" &&
                  mounted->Text() == "F2 / S",
              "authored Runtime UI shows the current binding in source order");
    }
    for (const std::string type : {"Label", "RichTextLabel", "Button", "IconButton"}) {
        px::sdk::UiDocument captions;
        captions.id = "11000000-0000-4000-8000-000000000001";
        captions.rootId = "11000000-0000-4000-8000-000000000002";
        px::sdk::UiNode root;
        root.id = captions.rootId;
        root.kind = px::sdk::UiNodeKind::Group;
        root.runtimeType = "Panel";
        root.layout.width = 640;
        root.layout.height = 360;
        px::sdk::UiNode caption;
        caption.id = "11000000-0000-4000-8000-000000000003";
        caption.parentId = root.id;
        caption.kind = type.find("Button") != std::string::npos
                           ? px::sdk::UiNodeKind::Button : px::sdk::UiNodeKind::Label;
        caption.runtimeType = type;
        caption.text = "SAVE / 小光";
        caption.appearance.textColor = "#123456FF";
        caption.accessibilityLabel = "Separate accessible name";
        caption.layout.width = 320;
        caption.layout.height = 80;
        if (type == "Label" || type == "RichTextLabel") {
            caption.runtimeProperties.emplace("outlineSize", std::int64_t{2});
            caption.runtimeProperties.emplace(
                "outlineColor", px::sdk::UiColorValue{"#2D171DFF"});
        }
        captions.nodes = {root, caption};
        const auto checkCaption = [&](const std::string& expected) {
            auto tree = px::ui::BuildUiRuntimeTree(captions);
            Check(tree.Valid(), "typed caption fixture must adapt");
            const auto* control = tree.root->Find(*px::Uuid::Parse(caption.id));
            const auto* label = dynamic_cast<const px::ui::Label*>(control);
            const auto* button = dynamic_cast<const px::ui::Button*>(control);
            Check((label && label->Text() == expected) ||
                      (button && button->Text() == expected),
                  "runtimeType must retain authored visible text independently of accessibility");
            if (const auto* rich = dynamic_cast<const px::ui::RichTextLabel*>(control))
                Check(rich->Markup() == expected, "typed rich caption must initialize markup as well as text");
            if (label) {
                const auto color = label->GetColor();
                Check(color.r == 0x12 && color.g == 0x34 && color.b == 0x56 &&
                          color.a == 0xFF,
                      "runtimeType label must retain authored appearance textColor");
                const auto edge = label->OutlineColor();
                Check(label->OutlineSize() == 2 && edge.r == 0x2D &&
                          edge.g == 0x17 && edge.b == 0x1D && edge.a == 0xFF,
                      "typed label outline must survive UI runtime adaptation");
            }
            if (button) {
                const auto color = button->GetTextColor();
                Check(button->HasTextColor() && color.r == 0x12 && color.g == 0x34 &&
                          color.b == 0x56 && color.a == 0xFF,
                      "runtimeType button must retain authored appearance textColor");
            }
        };
        checkCaption(caption.text);
        captions.nodes[1].runtimeProperties.emplace(type == "RichTextLabel" ? "markup" : "text", "Explicit text override");
        checkCaption("Explicit text override");
    }
    std::string scene = R"({
      "format":"PrismatiXUIScene","schemaRevision":1,
      "id":"11111111-1111-4111-8111-111111111111","revision":9,
      "name":"Title","width":1280,"height":720,
      "rootId":"22222222-2222-4222-8222-222222222222",
      "nodes":[
        {"id":"22222222-2222-4222-8222-222222222222","parentId":null,"order":0,
         "kind":"control","name":"Root","visible":true,"locked":false,
         "layout":{"mode":"free","x":0,"y":0,"width":1280,"height":720,"anchorX":0,"anchorY":0,"pivotX":0,"pivotY":0,"margin":0,"alignment":"start","sizeRule":"fixed"},
         "content":{"text":"","assetId":null},"appearance":{"backgroundColor":"#16121A","textColor":"#FFFFFF","opacity":1,"styleToken":null,"hoverBackgroundColor":null,"focusColor":null,"disabledOpacity":0.5},
         "interaction":{"onClick":null},"accessibility":{"label":"","role":"presentation"}},
        {"id":"33333333-3333-4333-8333-333333333333","parentId":"22222222-2222-4222-8222-222222222222","order":0,
         "kind":"button","name":"Start","visible":true,"locked":false,
         "layout":{"mode":"free","x":-120,"y":-30,"width":240,"height":60,"anchorX":0.5,"anchorY":0.5,"pivotX":0.5,"pivotY":0.5,"margin":0,"alignment":"center","sizeRule":"fixed"},
         "content":{"text":"Start","assetId":null},"appearance":{"backgroundColor":"#F052A0","textColor":"#FFFFFF","opacity":1,"styleToken":null,"hoverBackgroundColor":"#FF79BA","focusColor":"#FFFFFF","disabledOpacity":0.5},
         "interaction":{"onClick":{"id":"game.start","arguments":{}}},"accessibility":{"label":"Start game","role":"button"}},
        {"id":"55555555-5555-4555-8555-555555555555","parentId":"22222222-2222-4222-8222-222222222222","order":1,
         "kind":"image","name":"Logo","visible":true,"locked":false,
         "layout":{"mode":"free","x":40,"y":40,"width":320,"height":120,"anchorX":0,"anchorY":0,"pivotX":0,"pivotY":0,"margin":0,"alignment":"start","sizeRule":"fixed"},
         "content":{"text":"","assetId":"66666666-6666-4666-8666-666666666666"},"appearance":{"backgroundColor":"#00000000","textColor":"#FFFFFF","opacity":0.75,"styleToken":null,"hoverBackgroundColor":null,"focusColor":null,"disabledOpacity":0.5},
         "interaction":{"onClick":null},"accessibility":{"label":"Logo","role":"image"}}
      ],"theme":[]
    })";
    const auto end = scene.rfind("\n    }");
    Check(end != std::string::npos, "base Runtime fixture must have an object end");
    scene.insert(end, R"(,
      "behaviorGraph":{
        "nodes":[
          {"id":"77777777-7777-4777-8777-777777777777","kind":"signalEntry",
           "position":{"x":80,"y":120},"properties":{},"arguments":{}},
          {"id":"88888888-8888-4888-8888-888888888888","kind":"setProperty",
           "position":{"x":360,"y":120},
           "properties":{"target":{"type":"nodeReference","nodeId":"33333333-3333-4333-8333-333333333333"},"property":"opacity","value":0.25},
           "arguments":{}},
          {"id":"89898989-8989-4989-8989-898989898989","kind":"delay",
           "position":{"x":640,"y":120},"properties":{"seconds":10},"arguments":{}}
        ],
        "links":[
          {"id":"99999999-9999-4999-8999-999999999999",
           "fromNodeId":"77777777-7777-4777-8777-777777777777","fromPin":"out",
           "toNodeId":"88888888-8888-4888-8888-888888888888","toPin":"in"},
          {"id":"98989898-9898-4989-8989-989898989898",
           "fromNodeId":"88888888-8888-4888-8888-888888888888","fromPin":"out",
           "toNodeId":"89898989-8989-4989-8989-898989898989","toPin":"in"}
        ],"groups":[]
      },
      "behaviorTriggers":[
        {"id":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
         "nodeId":"33333333-3333-4333-8333-333333333333","signal":"activated",
         "entryNodeId":"77777777-7777-4777-8777-777777777777","reentry":"restart"}
      ],
      "animations":{
        "clips":[
          {"id":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb","name":"Pulse",
           "duration":1,"loop":true,
           "tracks":[
             {"id":"cccccccc-cccc-4ccc-8ccc-cccccccccccc",
              "nodeId":"33333333-3333-4333-8333-333333333333","property":"opacity",
              "keys":[
                {"id":"dddddddd-dddd-4ddd-8ddd-dddddddddddd","time":0,"value":1,
                 "easing":"linear","interpolation":"linear"},
                {"id":"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee","time":1,"value":0.4,
                 "easing":"easeInOut","interpolation":"linear"}
              ]}
           ]}
        ],
        "stateMachine":{
          "entryStateId":"12121212-1212-4212-8212-121212121212",
          "parameters":[],
          "states":[
            {"id":"12121212-1212-4212-8212-121212121212","name":"Idle",
             "clipId":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
             "position":{"x":80,"y":80}}
          ],
          "transitions":[]
        }
      })");
    scene = CanonicalUiText(scene);
    const auto parsed = px::sdk::ParseUi(scene);
    Check(parsed.Valid(), "valid UI document must parse before Runtime adaptation");
    auto typedActionScene = nlohmann::json::parse(scene);
    for (auto& node : typedActionScene["nodes"]) {
        if (node["kind"] != "button") continue;
        node["onClick"] = {
            {"id", "extension.test"},
            {"arguments", {
                {"tint", {{"type", "color"}, {"value", "#ffccdd"}}},
                {"target", {{"type", "uuid"}, {"value", "22222222-2222-4222-8222-222222222222"}}},
                {"media", {{"type", "resource"}, {"value", "Assets/Audio/theme.ogg"}}},
                {"token", {{"type", "token"}, {"value", "accent"}}},
                {"items", nlohmann::json::array({1, "two", true})},
                {"options", {{"volume", 0.5}, {"loop", true}}},
            }},
        };
    }
    const auto typedAction = px::sdk::ParseUi(typedActionScene.dump());
    Check(typedAction.Valid(), "typed Action arguments must parse in authored UI");
    const auto actionNode = std::ranges::find_if(typedAction.document.nodes,
        [](const px::sdk::UiNode& node) { return node.kind == px::sdk::UiNodeKind::Button; });
    Check(actionNode != typedAction.document.nodes.end() && actionNode->onClick.has_value(),
          "typed Action button must retain its invocation");
    const auto& typedArguments = actionNode->onClick->arguments;
    const auto tint = px::ui::ConvertUiActionValue(typedArguments.at("tint"));
    const auto target = px::ui::ConvertUiActionValue(typedArguments.at("target"));
    const auto media = px::ui::ConvertUiActionValue(typedArguments.at("media"));
    const auto token = px::ui::ConvertUiActionValue(typedArguments.at("token"));
    const auto items = px::ui::ConvertUiActionValue(typedArguments.at("items"));
    const auto options = px::ui::ConvertUiActionValue(typedArguments.at("options"));
    Check(tint && tint->Type() == px::VariantType::Color &&
          target && target->Type() == px::VariantType::Uuid &&
          media && media->Type() == px::VariantType::ResourceRef &&
          media->TryGet<px::ResourceRefValue>()->lastKnownPath == "Assets/Audio/theme.ogg" &&
          token && token->Type() == px::VariantType::TokenRef &&
          items && items->Type() == px::VariantType::Array &&
          options && options->Type() == px::VariantType::Object,
          "typed Action values must reach the Runtime with their declared Variant types");
    px::ui::ActionCatalog typedCatalog;
    px::ui::ActionDescriptor typedDescriptor;
    typedDescriptor.id = "extension.test";
    typedDescriptor.arguments = {
        {.name = "tint", .type = px::VariantType::Color, .required = true},
        {.name = "target", .type = px::VariantType::Uuid, .required = true},
        {.name = "media", .type = px::VariantType::ResourceRef, .required = true},
        {.name = "token", .type = px::VariantType::TokenRef, .required = true},
        {.name = "items", .type = px::VariantType::Array, .required = true},
        {.name = "options", .type = px::VariantType::Object, .required = true},
    };
    Check(typedCatalog.Register(std::move(typedDescriptor)).IsOk(),
          "typed Action descriptor must register");
    px::ui::ActionInvocation typedInvocation;
    typedInvocation.action = "extension.test";
    typedInvocation.arguments = {
        {"tint", tint->Clone()}, {"target", target->Clone()},
        {"media", media->Clone()}, {"token", token->Clone()},
        {"items", items->Clone()}, {"options", options->Clone()},
    };
    Check(static_cast<bool>(typedCatalog.ValidateAndNormalize(typedInvocation)),
          "converted UI arguments must satisfy the production ActionCatalog");
    auto unsafeActionScene = typedActionScene;
    for (auto& node : unsafeActionScene["nodes"])
        if (node["kind"] == "button")
            node["onClick"]["arguments"]["media"]["value"] = "../secret.ogg";
    Check(!px::sdk::ParseUi(unsafeActionScene.dump()).Valid(),
          "typed Action resources must reject unsafe project paths");
    std::size_t actions = 0;
    auto runtime = px::ui::BuildUiRuntimeTree(
        parsed.document,
        [](const std::string_view id) -> std::optional<std::string> {
            if (id == "66666666-6666-4666-8666-666666666666") return "Content/logo.png";
            return std::nullopt;
        },
        [&actions](const px::sdk::UiAction& action, std::string_view, std::string_view) {
            Check(action.id == "game.start", "Runtime button must dispatch the authored typed action");
            ++actions;
        }
    );
    Check(runtime.Valid(), "UI document must produce a Runtime tree");
    Check(runtime.nodeCount == 3, "Runtime tree must retain every authored node");
    Check(runtime.actionBindingCount == 1, "Runtime tree must bind the authored button action");
    Check(runtime.behaviorNodeCount == 3 && runtime.behaviorTriggerCount == 1, "Runtime adapter must retain Behavior Graph nodes and triggers");
    Check(runtime.animationClipCount == 1 && runtime.animationTrackCount == 1, "Runtime adapter must retain Animation clips and tracks");
    auto optionScene = nlohmann::json::parse(scene);
    optionScene["schemaRevision"] = 2;
    optionScene["theme"].push_back({ { "id", "opacity_soft" }, { "name", "Soft opacity" }, { "value", "0.35" } });
    optionScene["theme"].push_back({ { "id", "tint_soft" }, { "name", "Soft tint" }, { "value", "#336699" } });
    optionScene["nodes"].push_back(
        { { "id", "17171717-1717-4717-8717-171717171717" },
          { "parentId", "22222222-2222-4222-8222-222222222222" },
          { "order", 2 },
          { "kind", "leaf" },
          { "runtimeType", "OptionButton" },
          { "name", "Chapter" },
          { "visible", true },
          { "locked", false },
          { "layout", { { "mode", "free" }, { "x", 420 }, { "y", 40 }, { "width", 220 }, { "height", 48 }, { "anchorX", 0 }, { "anchorY", 0 }, { "pivotX", 0 }, { "pivotY", 0 }, { "margin", 0 }, { "alignment", "start" }, { "sizeRule", "fixed" } } },
          { "content", { { "text", "" }, { "assetId", nullptr } } },
          { "appearance", { { "backgroundColor", "#202030" }, { "textColor", "#FFFFFF" }, { "opacity", 1 }, { "styleToken", nullptr }, { "hoverBackgroundColor", nullptr }, { "focusColor", nullptr }, { "disabledOpacity", 0.5 } } },
          { "interaction", { { "onClick", nullptr } } },
          { "accessibility", { { "label", "Chapter" }, { "role", "combobox" } } },
          { "runtimeProperties", { { "options", nlohmann::json::array({ "Prologue", "Finale" }) } } },
          { "bindings", { { "text", { { "path", "dialogue.text" } } } } } }
    );
    optionScene["nodes"].push_back(
        { { "id", "18181818-1818-4818-8818-181818181818" },
          { "parentId", "22222222-2222-4222-8222-222222222222" },
          { "order", 3 },
          { "kind", "image" },
          { "runtimeType", "TextureRect" },
          { "name", "TitleTexture" },
          { "visible", true },
          { "locked", false },
          { "layout", { { "mode", "free" }, { "x", 680 }, { "y", 40 }, { "width", 240 }, { "height", 120 }, { "anchorX", 0 }, { "anchorY", 0 }, { "pivotX", 0 }, { "pivotY", 0 }, { "margin", 0 }, { "alignment", "start" }, { "sizeRule", "fixed" } } },
          { "content", { { "text", "" }, { "assetId", nullptr } } },
          { "appearance", { { "backgroundColor", "#202030" }, { "textColor", "#FFFFFF" }, { "opacity", 1 }, { "styleToken", nullptr }, { "hoverBackgroundColor", nullptr }, { "focusColor", nullptr }, { "disabledOpacity", 0.5 } } },
          { "interaction", { { "onClick", nullptr } } },
          { "accessibility", { { "label", "Title" }, { "role", "image" } } },
          { "runtimeProperties",
            { { "texture", { { "type", "resource" }, { "value", "66666666-6666-4666-8666-666666666666" } } },
              { "styleToken", { { "type", "token" }, { "value", "accent" } } },
              { "opacity", { { "type", "token" }, { "value", "opacity_soft" } } },
              { "modulate", { { "type", "token" }, { "value", "tint_soft" } } } } } }
    );
    optionScene["behaviorGraph"]["nodes"].push_back(
        { { "id", "21212121-2121-4212-8212-212121212121" }, { "kind", "signalEntry" }, { "position", { { "x", 80 }, { "y", 360 } } }, { "properties", nlohmann::json::object() }, { "arguments", nlohmann::json::object() } }
    );
    optionScene["behaviorGraph"]["nodes"].push_back(
        { { "id", "23232323-2323-4232-8232-232323232323" }, { "kind", "delay" }, { "position", { { "x", 320 }, { "y", 360 } } }, { "properties", { { "seconds", 0 } } }, { "arguments", nlohmann::json::object() } }
    );
    optionScene["behaviorGraph"]["links"].push_back(
        { { "id", "24242424-2424-4242-8242-242424242424" }, { "fromNodeId", "21212121-2121-4212-8212-212121212121" }, { "fromPin", "out" }, { "toNodeId", "23232323-2323-4232-8232-232323232323" }, { "toPin", "in" } }
    );
    optionScene["behaviorGraph"]["links"].push_back(
        { { "id", "25252525-2525-4252-8252-252525252525" }, { "fromNodeId", "21212121-2121-4212-8212-212121212121" }, { "fromPin", "arg:index" }, { "toNodeId", "23232323-2323-4232-8232-232323232323" }, { "toPin", "seconds" } }
    );
    optionScene["behaviorTriggers"].push_back(
        { { "id", "26262626-2626-4262-8262-262626262626" }, { "nodeId", "17171717-1717-4717-8717-171717171717" }, { "signal", "itemSelected" }, { "entryNodeId", "21212121-2121-4212-8212-212121212121" }, { "reentry", "restart" } }
    );
    optionScene = CanonicalUi(std::move(optionScene));
    const auto optionParsed = px::sdk::ParseUi(optionScene.dump());
    Check(optionParsed.Valid(), "dynamic OptionButton scene must satisfy the SDK contract");
    auto invalidBehaviorSignal = optionParsed.document;
    invalidBehaviorSignal.behaviorTriggers.back().signal = "missingSignal";
    const auto invalidBehaviorRuntime =
        px::ui::BuildUiRuntimeTree(invalidBehaviorSignal, [](const std::string_view id) -> std::optional<std::string> { return id == "66666666-6666-4666-8666-666666666666" ? std::optional<std::string>{ "Content/logo.png" } : std::nullopt; });
    Check(
        !invalidBehaviorRuntime.Valid() && std::ranges::any_of(invalidBehaviorRuntime.diagnostics, [](const px::ui::UiAdapterDiagnostic& diagnostic) { return diagnostic.code == "PXUISTUDIO2010"; }),
        "dynamic Behavior signals must fail closed against the Runtime TypeRegistry"
    );
    px::ui::ObservableViewModel optionViewModel;
    px::ui::UIContext optionContext;
    Check(static_cast<bool>(optionViewModel.Define("dialogue.text", px::Variant(std::string{ "Bound Chapter" }), true)), "binding test ViewModel must define dialogue.text");
    px::ui::UiApplication optionApplication(optionContext);
    const auto optionApplied = optionApplication.ApplyDocument(
        optionParsed.document,
        { .sourcePath = "Content/UI/Option.pxui",
          .resolveAsset = [](const std::string_view id) -> std::optional<std::string> { return id == "66666666-6666-4666-8666-666666666666" ? std::optional<std::string>{ "Content/logo.png" } : std::nullopt; },
          .viewModel = &optionViewModel }
    );
    const auto optionId = px::Uuid::Parse("17171717-1717-4717-8717-171717171717");
    auto* optionButton = optionApplied && optionId ? dynamic_cast<px::ui::OptionButton*>(optionContext.Root()->Find(*optionId)) : nullptr;
    const auto textureId = px::Uuid::Parse("18181818-1818-4818-8818-181818181818");
    auto* referencedTexture = optionApplied && textureId ? dynamic_cast<px::ui::TextureRect*>(optionContext.Root()->Find(*textureId)) : nullptr;
    if (!optionApplied)
        for (const auto& diagnostic : optionApplied.Diagnostics()) std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    else if (!optionButton && optionId) {
        const auto* control = optionContext.Root()->Find(*optionId);
        std::cerr << "Option Runtime type: " << (control ? control->TypeName() : std::string_view{ "missing" }) << '\n';
    }
    Check(
        optionApplied && optionButton && optionButton->Options() == std::vector<std::string>{ "Prologue", "Finale" } && optionButton->Text() == "Bound Chapter" && optionApplied.Value().propertyBindingCount == 1,
        "shared Preview/Player application must instantiate OptionButton and apply its typed binding"
    );
    Check(optionViewModel.Write("dialogue.text", px::Variant(std::string{ "Updated Chapter" })) && optionButton && optionButton->Text() == "Updated Chapter", "shared Runtime binding must retain its subscription and call the real text setter");
    auto indexedOptionScene = optionScene;
    for (auto& node : indexedOptionScene["nodes"])
        if (node["id"] == "17171717-1717-4717-8717-171717171717")
            node["bindings"]["text"]["path"] = "saveLoad.slots[0].title";
    const auto indexedParsed = px::sdk::ParseUi(indexedOptionScene.dump());
    Check(indexedParsed.Valid(), "indexed ViewModel binding must parse as a canonical UI scene");
    px::ui::ObservableViewModel indexedViewModel;
    Check(static_cast<bool>(indexedViewModel.Define(
              "saveLoad.slots",
              px::Variant(px::VariantArray{px::Variant(px::VariantObject{
                  {"title", std::string{"Saved One"}}})}))),
          "one typed collection must replace per-cell ViewModel declarations");
    px::ui::UIContext indexedContext;
    px::ui::UiApplication indexedApplication(indexedContext);
    const auto indexedApplied = indexedApplication.ApplyDocument(
        indexedParsed.document,
        {.sourcePath = "Content/UI/IndexedOption.pxui",
         .resolveAsset = [](const std::string_view id) -> std::optional<std::string> {
             return id == "66666666-6666-4666-8666-666666666666"
                        ? std::optional<std::string>{"Content/logo.png"}
                        : std::nullopt;
         },
         .viewModel = &indexedViewModel});
    auto* indexedButton = indexedApplied && optionId
        ? dynamic_cast<px::ui::OptionButton*>(indexedContext.Root()->Find(*optionId))
        : nullptr;
    Check(indexedApplied && indexedButton && indexedButton->Text() == "Saved One",
          "indexed collection item must initialize a real UI property binding");
    Check(indexedViewModel.Write("saveLoad.slots[0].title", px::Variant("Saved Two")) &&
              indexedButton && indexedButton->Text() == "Saved Two",
          "indexed item writes must notify subscribed UI controls");
    Check(indexedViewModel.Write(
              "saveLoad.slots",
              px::Variant(px::VariantArray{px::Variant(px::VariantObject{
                  {"title", std::string{"Saved Three"}}})})) &&
              indexedButton && indexedButton->Text() == "Saved Three",
          "replacing a collection must refresh indexed item bindings");
    Check(!indexedViewModel.Write("saveLoad.slots", px::Variant(px::VariantArray{})) &&
              !indexedViewModel.Write("saveLoad.slots", px::Variant(px::VariantArray{
                  px::Variant(px::VariantObject{{"title", px::Variant(false)}})})) &&
              indexedButton && indexedButton->Text() == "Saved Three",
          "collection replacements must not silently leave bound controls stale");
    Check(!indexedViewModel.Describe("saveLoad.slots[1].title") &&
              !indexedViewModel.Describe("saveLoad.slots[00].title") &&
              !indexedViewModel.Write("saveLoad.slots[0].title", px::Variant(false)),
          "invalid indexes and incompatible nested writes must fail closed");
    Check(
        referencedTexture && referencedTexture->Texture().id.ToString() == "66666666-6666-4666-8666-666666666666" && referencedTexture->Texture().lastKnownPath == "Content/logo.png" && referencedTexture->Path() == "Content/logo.png" &&
            referencedTexture->StyleToken().name == "accent" && referencedTexture->Opacity() == 0.35f && referencedTexture->Modulate() == px::Color{ 0x33, 0x66, 0x99, 0xff },
        "shared adapter must resolve ResourceRef and typed theme tokens through real setters"
    );
    px::ui::UIEvent optionSelection{ .type = px::ui::UIEventType::Activate };
    optionButton->HandleEvent(optionSelection);
    const auto optionBehavior = optionContext.CaptureBehaviorState();
    const auto optionEntryId = px::Uuid::Parse("21212121-2121-4212-8212-212121212121");
    const auto optionDelayId = px::Uuid::Parse("23232323-2323-4232-8232-232323232323");
    const auto behaviorFiber = std::ranges::find_if(optionBehavior.fibers, [&](const px::ui::BehaviorFiberState& fiber) { return optionEntryId && fiber.entry == *optionEntryId; });
    const auto indexArgument = behaviorFiber == optionBehavior.fibers.end() ? nullptr : behaviorFiber->signalArguments.at("index").TryGet<std::int64_t>();
    const auto textArgument = behaviorFiber == optionBehavior.fibers.end() ? nullptr : behaviorFiber->signalArguments.at("text").TryGet<std::string>();
    Check(
        optionEntryId && optionDelayId && behaviorFiber != optionBehavior.fibers.end() && behaviorFiber->current == *optionDelayId && behaviorFiber->delayRemaining == 1.0f && indexArgument && *indexArgument == 1 && textArgument &&
            *textArgument == "Finale",
        "OptionButton.itemSelected(index,text) must preserve live typed arguments in the Behavior entry context"
    );
    auto complexDocument = parsed.document;
    auto appendConstant = [&](const std::string& id, px::sdk::UiValue value) {
        auto node = complexDocument.behaviorGraph.nodes.front();
        node.id = id;
        node.kind = px::sdk::UiBehaviorNodeKind::Constant;
        node.properties.clear();
        node.properties.emplace("value", std::move(value));
        node.arguments.clear();
        complexDocument.behaviorGraph.nodes.push_back(std::move(node));
    };
    appendConstant("13131313-1313-4313-8313-131313131313", px::sdk::UiUuidValue{ "a6c8947c-460f-42ef-a8ea-3a420e80df99" });
    appendConstant("14141414-1414-4414-8414-141414141414", px::sdk::UiResourceValue{ "a6c8947c-460f-42ef-a8ea-3a420e80df99" });
    appendConstant("15151515-1515-4515-8515-151515151515", px::sdk::UiTokenValue{ "accent.primary" });
    appendConstant("16161616-1616-4616-8616-161616161616", px::sdk::UiArrayValue{ R"([0.25,0.75])" });
    appendConstant("17171717-1717-4717-8717-171717171717", px::sdk::UiObjectValue{ R"({"difficulty":"normal","flags":[true,false]})" });
    auto complexRuntime = px::ui::BuildUiRuntimeTree(complexDocument);
    Check(complexRuntime.Valid() && complexRuntime.behaviorGraph.has_value(), "semantic and recursive SDK values must adapt into Runtime Variants");
    const auto runtimeValueType = [&](const std::string_view id) {
        const auto parsedId = px::Uuid::Parse(id);
        const auto found = std::ranges::find_if(complexRuntime.behaviorGraph->nodes, [&](const px::ui::BehaviorNode& node) { return parsedId && node.id == *parsedId; });
        Check(found != complexRuntime.behaviorGraph->nodes.end(), "complex Runtime value fixture node must exist");
        return found->properties.at("value").Type();
    };
    Check(
        runtimeValueType("13131313-1313-4313-8313-131313131313") == px::VariantType::Uuid && runtimeValueType("14141414-1414-4414-8414-141414141414") == px::VariantType::ResourceRef &&
            runtimeValueType("15151515-1515-4515-8515-151515151515") == px::VariantType::TokenRef && runtimeValueType("16161616-1616-4616-8616-161616161616") == px::VariantType::Array &&
            runtimeValueType("17171717-1717-4717-8717-171717171717") == px::VariantType::Object,
        "Runtime adapter must preserve UUID, ResourceRef, TokenRef, Array and Object types"
    );
    Check(runtime.unresolvedAssetIds.empty(), "known asset must resolve");
    const auto buttonId = px::Uuid::Parse("33333333-3333-4333-8333-333333333333");
    const auto imageId = px::Uuid::Parse("55555555-5555-4555-8555-555555555555");
    Check(buttonId.has_value() && imageId.has_value(), "fixture UUIDs must parse");
    auto* button = dynamic_cast<px::ui::Button*>(runtime.root->Find(*buttonId));
    const auto* image = dynamic_cast<const px::ui::TextureRect*>(runtime.root->Find(*imageId));
    Check(button != nullptr && image != nullptr, "Runtime tree must use real Button and TextureRect controls");
    Check(button->Anchors() == px::Rect{ 0.5f, 0.5f, 0.5f, 0.5f }, "free-layout anchors must reach the Runtime control");
    Check(button->Offsets() == px::Rect{ -120, -30, 240, 60 }, "free-layout offsets must remain relative to the authored anchor");
    (void)runtime.root->Measure({ 1280, 720 });
    runtime.root->Arrange({ 0, 0, 1280, 720 });
    Check(button->LayoutRect() == px::Rect{ 520, 330, 240, 60 }, "normalized anchors must reposition the control against its parent");

    auto compactDocument = parsed.document;
    auto compactNode = std::ranges::find_if(
        compactDocument.nodes, [](const px::sdk::UiNode& node) {
            return node.name == "Start";
        });
    Check(compactNode != compactDocument.nodes.end(),
          "compact fixed-layout fixture button must exist");
    compactNode->layout.x = 739.0f;
    compactNode->layout.y = 680.0f;
    compactNode->layout.width = 43.0f;
    compactNode->layout.height = 17.0f;
    compactNode->layout.anchorX = compactNode->layout.anchorRight = 0.0f;
    compactNode->layout.anchorY = compactNode->layout.anchorBottom = 0.0f;
    auto compactRuntime = px::ui::BuildUiRuntimeTree(compactDocument);
    auto* compactButton = compactRuntime.Valid()
                              ? dynamic_cast<px::ui::Button*>(
                                    compactRuntime.root->Find(*buttonId))
                              : nullptr;
    Check(compactButton != nullptr,
          "compact authored button must instantiate as a Runtime button");
    (void)compactRuntime.root->Measure({1280, 720});
    compactRuntime.root->Arrange({0, 0, 1280, 720});
    Check(compactButton->LayoutRect() == px::Rect{739, 680, 43, 17},
          "fixed free-layout rectangles must not expand to a control's intrinsic minimum");

    auto responsiveSource = nlohmann::json::parse(scene);
    responsiveSource["schemaRevision"] = 2;
    auto& responsiveLayout = responsiveSource["nodes"][1]["layout"];
    responsiveLayout["x"] = 0;
    responsiveLayout["y"] = 0;
    responsiveLayout["width"] = 640;
    responsiveLayout["height"] = 360;
    responsiveLayout["anchorX"] = 0.25;
    responsiveLayout["anchorY"] = 0.25;
    responsiveLayout["anchorRight"] = 0.75;
    responsiveLayout["anchorBottom"] = 0.75;
    const auto responsiveParsed = px::sdk::ParseUi(responsiveSource.dump());
    Check(responsiveParsed.Valid(), "revision-2 responsive constraint fixture must parse");
    auto responsiveRuntime = px::ui::BuildUiRuntimeTree(responsiveParsed.document);
    Check(responsiveRuntime.Valid(), "revision-2 edge constraints must build a Runtime tree");
    auto* responsiveButton = dynamic_cast<px::ui::Button*>(responsiveRuntime.root->Find(*buttonId));
    Check(responsiveButton != nullptr && responsiveButton->Anchors() == px::Rect{ 0.25f, 0.25f, 0.75f, 0.75f } && responsiveButton->Offsets() == px::Rect{ 0, 0, 0, 0 }, "Studio constraints must translate to Runtime anchors and offsets");
    (void)responsiveRuntime.root->Measure({ 1600, 900 });
    responsiveRuntime.root->Arrange({ 0, 0, 1600, 900 });
    Check(responsiveButton->LayoutRect() == px::Rect{ 400, 225, 800, 450 }, "Runtime edge constraints must stretch against a resized parent");
    Check(image->Path() == "Content/logo.png", "image path must use the resolver");
    Check(image->Opacity() == 0.75f, "image opacity must round-trip");
    button->Activate();
    Check(actions == 1, "button activation must reach the Preview action sink");

    Check(px::ui::RegisterBuiltinUITypes().IsOk(), "Runtime UI property registry must be available");
    px::ui::UIContext context;
    Check(context.SetRoot(std::move(runtime.root)).IsOk(), "Runtime UIContext must install the Studio tree");
    Check(runtime.animations.has_value() && context.SetAnimations(std::move(*runtime.animations), true).IsOk(), "Runtime UIContext must install the Studio animation library");
    Check(context.ConfigureTriggers(std::move(runtime.behaviorTriggers), std::move(runtime.behaviorGraph), "fixture.pxui").IsOk(), "Runtime UIContext must install Studio Behavior triggers");
    const auto idleStateId = px::Uuid::Parse("12121212-1212-4212-8212-121212121212");
    const auto delayNodeId = px::Uuid::Parse("89898989-8989-4989-8989-898989898989");
    Check(idleStateId && context.CaptureAnimationState().state == *idleStateId, "Runtime UIContext must expose the active authored Animation state");
    button = dynamic_cast<px::ui::Button*>(context.Root()->Find(*buttonId));
    Check(button != nullptr, "installed Runtime button must remain typed");
    button->Activate();
    Check(button->Opacity() == 0.25f, "Behavior trigger must execute through the real Runtime graph");
    const auto activeBehavior = context.CaptureBehaviorState();
    Check(delayNodeId && activeBehavior.fibers.size() == 1 && activeBehavior.fibers.front().current == *delayNodeId, "Runtime UIContext must expose the active authored Behavior node");
    const auto clipId = px::Uuid::Parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    Check(clipId.has_value() && context.PreviewAnimation(*clipId, 1.0f, false).IsOk(), "Runtime animation controller must preview the authored clip");
    Check(button->Opacity() == 0.4f, "Animation sampling must update the real Runtime property");

    px::ui::UIContext applicationContext;
    std::size_t applicationActions = 0;
    Check(
        applicationContext.Commands()
            .Register(
                "game.start",
                [&applicationActions](const px::Variant&) {
                    ++applicationActions;
                    return px::Status::Ok();
                }
            )
            .IsOk(),
        "headless Player-style command route must register"
    );
    px::ui::UiApplication application(applicationContext);
    auto applied = application.ApplyText(scene, { .sourcePath = "Content/UI/Title.pxui", .resolveAsset = [](const std::string_view id) -> std::optional<std::string> {
                                                     return id == "66666666-6666-4666-8666-666666666666" ? std::optional<std::string>{ "Content/logo.png" } : std::nullopt;
                                                 } });
    Check(static_cast<bool>(applied), "shared UI document application path must install the document");
    Check(applied.Value().nodeCount == 3 && applied.Value().behaviorNodeCount == 3 && applied.Value().animationClipCount == 1, "shared application reports the installed Runtime responsibilities");
    auto* applicationButton = dynamic_cast<px::ui::Button*>(applicationContext.Root()->Find(*buttonId));
    Check(applicationButton != nullptr, "shared application exposes the same typed Runtime controls");
    applicationButton->Activate();
    Check(applicationActions == 1, "shared application dispatches authored Actions through UIContext");
    Check(applicationButton->Opacity() == 0.25f, "shared application installs Behavior using the same activation");

    auto motionDocument = parsed.document;
    auto motionNode = std::ranges::find_if(motionDocument.nodes,
        [](const px::sdk::UiNode& node) { return node.name == "Logo"; });
    Check(motionNode != motionDocument.nodes.end(), "motion test image node must exist");
    motionNode->runtimeType = "MotionRect";
    motionNode->assetId.reset();
    motionNode->runtimeProperties["path"] = std::string{"Content/Motion/logo.pxmotion"};
    constexpr auto motionSource = R"({
      "format":"PrismatiXMotion","schemaRevision":3,"id":"logo",
      "canvas":{"width":320,"height":120},"duration":1,"loop":false,
      "layers":[{"id":"body","image":"Assets/body.png","width":100,"height":100}],
      "markers":[{"id":"first","time":0.25,"audio":"Assets/first.ogg","bus":"se"},
                 {"id":"second","time":0.75,"audio":"Assets/second.ogg","bus":"voice"}],
      "tracks":[{"layer":"body","property":"opacity","keys":[
        {"time":0,"value":0,"curve":"linear"},
        {"time":1,"value":1,"curve":"linear"}]}]})";
    px::ui::UIContext motionContext;
    std::vector<std::string> firedMotionMarkers;
    Check(motionContext.Commands().Register("game.start",
        [](const px::Variant&) { return px::Status::Ok(); }).IsOk(),
        "motion test action route must register");
    px::ui::UiApplication motionApplication(motionContext);
    auto motionApplied = motionApplication.ApplyDocument(motionDocument,
        {.sourcePath = "Content/UI/Title.pxui",
         .loadMotion = [](const std::string_view path) -> std::optional<std::string> {
             return path == "Content/Motion/logo.pxmotion"
                 ? std::optional<std::string>{motionSource} : std::nullopt;
         }});
    Check(static_cast<bool>(motionApplied), "authored MotionRect must load through UiApplication");
    auto* motion = dynamic_cast<px::ui::MotionRect*>(motionContext.Root()->Find(*imageId));
    Check(motion && motion->Clip() && motion->Playing(),
          "MotionRect must install a playable layered clip");
    motionContext.SetMotionAudioSink([&](const px::sdk::MotionAudioMarker& marker) {
        firedMotionMarkers.push_back(marker.id);
    });
    motion->Update(0.5f);
    Check(std::abs(motion->Position() - 0.5) < 0.0001,
          "MotionRect playback clock must advance deterministically");
    Check(firedMotionMarkers == std::vector<std::string>{"first"},
          "MotionRect emits only audio markers crossed by normal playback");
    motion->Seek(0.9);
    Check(firedMotionMarkers == std::vector<std::string>{"first"},
          "explicit seeking never retroactively emits audio markers");
    motion->Seek(0.0);
    motion->Update(1.0f);
    Check(firedMotionMarkers == std::vector<std::string>{"first", "first", "second"},
          "restarted playback emits each crossed marker once in timeline order");
    motionContext.SetMotionAudioSink({});
    motion->Seek(0.0);
    motion->SetPlaying(true);
    motion->Update(0.5f);
    Check(firedMotionMarkers.size() == 3,
          "clearing the UI-context audio service detaches an existing MotionRect");
    constexpr auto loopingSource = R"({
      "format":"PrismatiXMotion","schemaRevision":3,"id":"looping",
      "canvas":{"width":100,"height":100},"duration":1,"loop":true,
      "layers":[{"id":"body","image":"Assets/body.png","width":100,"height":100}],
      "tracks":[],"markers":[{"id":"beat","time":0,"audio":"Assets/beat.ogg","bus":"se"}]})";
    px::ui::MotionRect loopingMotion;
    std::vector<std::string> loopMarkers;
    loopingMotion.SetMarkerSink([&](const px::sdk::MotionAudioMarker& marker) {
        loopMarkers.push_back(marker.id);
    });
    Check(loopingMotion.SetSourceLoader([](std::string_view) {
        return std::optional<std::string>{loopingSource};
    }).IsOk() && loopingMotion.SetPath("Content/Motion/loop.pxmotion").IsOk(),
        "looping MotionRect loads a revision-3 clip");
    loopingMotion.Update(2.1f);
    Check(loopMarkers == std::vector<std::string>{"beat", "beat", "beat"},
          "looping motion emits a time-zero cue once per iteration");
    loopingMotion.Seek(0.0);
    Check(loopMarkers.size() == 3, "looping motion seek remains silent");
    loopingMotion.Update(0.1f);
    Check(loopMarkers.size() == 4, "playback after seek re-arms the start cue");
    loopingMotion.Seek(0.3);
    Check(loopingMotion.SetSourceLoader([](std::string_view) {
        return std::optional<std::string>{motionSource};
    }).IsOk() && loopingMotion.Clip() && loopingMotion.Clip()->id == "logo" &&
              std::abs(loopingMotion.Position() - 0.3) < 0.0001,
          "replacing a motion loader reloads unchanged paths and retains playback position");
    const auto failedReload = loopingMotion.SetSourceLoader([](std::string_view) {
        return std::optional<std::string>{};
    });
    Check(!failedReload && std::ranges::any_of(failedReload.Diagnostics(),
              [](const px::diag::Diagnostic& diagnostic) {
                  return diagnostic.code == "PXUIMOTION1001";
              }) &&
              loopingMotion.Clip() && loopingMotion.Clip()->id == "logo",
          "failed motion loader replacement keeps the last playable clip");
    Check(loopingMotion.SetPath("Content/Motion/another.pxmotion").IsOk() &&
              loopingMotion.Clip() && loopingMotion.Clip()->id == "logo",
          "failed motion loader replacement also restores the prior loader");
    const auto* installedMotionRoot = motionContext.Root();
    const auto missingMotion = motionApplication.ApplyDocument(motionDocument,
        {.sourcePath = "Content/UI/Title.pxui"});
    Check(!missingMotion && HasDiagnostic(missingMotion, "PXUIMOTION1002") &&
              motionContext.Root() == installedMotionRoot,
          "missing motion loader must reject without replacing the installed UI");

    auto visualSceneJson = nlohmann::json::parse(scene);
    visualSceneJson["schemaRevision"] = 2;
    visualSceneJson["visualStateGroups"] = nlohmann::json::array({
        {{"id", "interaction"},
         {"defaultState", "normal"},
         {"states", nlohmann::json::array({
             {{"id", "normal"},
              {"overrides", nlohmann::json::array({
                  {{"nodeId", buttonId->ToString()}, {"property", "scale"},
                   {"value", {{"type", "vec2"}, {"x", 1.0}, {"y", 1.0}}}}
              })}},
             {{"id", "hover"},
              {"overrides", nlohmann::json::array({
                  {{"nodeId", buttonId->ToString()}, {"property", "scale"},
                   {"value", {{"type", "vec2"}, {"x", 1.2}, {"y", 1.2}}}}
              })}}
         })},
         {"transitions", nlohmann::json::array({
             {{"from", "normal"}, {"to", "hover"}, {"duration", 1.0},
              {"easing", "linear"},
              {"animationClipId", "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"}}
         })}},
        {{"id", "focus"},
         {"defaultState", "unfocused"},
         {"states", nlohmann::json::array({
             {{"id", "unfocused"},
              {"overrides", nlohmann::json::array({
                  {{"nodeId", buttonId->ToString()}, {"property", "rotation"},
                   {"value", 0.0}}
              })}},
             {{"id", "focused"},
              {"overrides", nlohmann::json::array({
                  {{"nodeId", buttonId->ToString()}, {"property", "rotation"},
                   {"value", 15.0}}
              })}}
         })},
         {"transitions", nlohmann::json::array()}}
    });
    const auto visualParsed = px::sdk::ParseUi(visualSceneJson.dump());
    Check(visualParsed.Valid() &&
              visualParsed.document.visualStateGroups.size() == 2,
          "canonical UI parser must retain Visual State Groups");
    px::ui::UIContext visualContext;
    px::ui::UiApplication visualApplication(visualContext);
    const auto visualApplied = visualApplication.ApplyDocument(
        visualParsed.document,
        {.sourcePath = "Content/UI/VisualStates.pxui",
         .resolveAsset = [](const std::string_view id)
             -> std::optional<std::string> {
             return id == "66666666-6666-4666-8666-666666666666"
                        ? std::optional<std::string>{"Content/logo.png"}
                        : std::nullopt;
         }});
    auto* visualButton = visualApplied
                             ? dynamic_cast<px::ui::Button*>(
                                   visualContext.Root()->Find(*buttonId))
                             : nullptr;
    Check(visualApplied && visualButton &&
              visualApplied.Value().visualStateGroupCount == 2 &&
              visualApplied.Value().visualStateCount == 4,
          "shared UI application must install all Visual State metadata");
    Check(visualContext.SetVisualState("interaction", "hover") &&
              visualContext.SetVisualState("focus", "focused"),
          "independent Visual State Groups must activate simultaneously");
    px::Input noInput;
    (void)visualContext.Update(noInput, 1280, 720, 0.5f);
    Check(std::abs(visualButton->Scale().x - 1.1f) < 0.001f &&
              std::abs(visualButton->Rotation() - 15.0f) < 0.001f,
          "Visual State transitions must use typed Runtime properties while another group remains active");
    const auto visualCheckpoint = visualContext.CaptureVisualState();
    (void)visualContext.Update(noInput, 1280, 720, 0.5f);
    Check(std::abs(visualButton->Scale().x - 1.2f) < 0.001f &&
              visualContext.RestoreVisualState(visualCheckpoint) &&
              std::abs(visualButton->Scale().x - 1.1f) < 0.001f &&
              visualContext.ActiveVisualState("focus") ==
                  std::optional<std::string_view>{"focused"},
          "Visual State checkpoints must restore exact transition progress and composed active groups");
    auto invalidVisualDocument = visualParsed.document;
    invalidVisualDocument.visualStateGroups.front().states.front()
        .overrides.front().property = "missingProperty";
    px::ui::UIContext invalidVisualContext;
    px::ui::UiApplication invalidVisualApplication(invalidVisualContext);
    const auto invalidVisual = invalidVisualApplication.ApplyDocument(
        invalidVisualDocument,
        {.sourcePath = "invalid-visual-state.pxui",
         .resolveAsset = [](const std::string_view id)
             -> std::optional<std::string> {
             return id == "66666666-6666-4666-8666-666666666666"
                        ? std::optional<std::string>{"Content/logo.png"}
                        : std::nullopt;
         }});
    Check(!invalidVisual && HasDiagnostic(invalidVisual, "PXUISTUDIO2134"),
          "Visual State overrides must fail closed against shared writable property metadata");

    auto invalidInstallation = visualParsed.document;
    Check(invalidInstallation.animations.has_value(),
          "transactional installation fixture must contain an animation library");
    invalidInstallation.animations->stateMachine.entryStateId =
        "abababab-abab-4bab-8bab-abababababab";
    auto* rootBeforeRejectedInstallation = applicationContext.Root();
    auto* buttonBeforeRejectedInstallation = applicationButton;
    const auto rejectedInstallation = application.ApplyDocument(
        invalidInstallation,
        {.sourcePath = "Content/UI/RejectedInstallation.pxui",
         .resolveAsset = [](const std::string_view id)
             -> std::optional<std::string> {
             return id == "66666666-6666-4666-8666-666666666666"
                        ? std::optional<std::string>{"Content/logo.png"}
                        : std::nullopt;
         }});
    buttonBeforeRejectedInstallation->Activate();
    Check(!rejectedInstallation &&
              applicationContext.Root() == rootBeforeRejectedInstallation &&
              applicationContext.Root()->Find(*buttonId) ==
                  buttonBeforeRejectedInstallation &&
              applicationActions == 2,
          "failed full document installation must retain the previous tree, Actions, and trigger connections");

    auto patchedSceneJson = nlohmann::json::parse(scene);
    patchedSceneJson["revision"] = 10;
    for (auto& node : patchedSceneJson["nodes"]) {
        if (node["id"] != buttonId->ToString()) continue;
        node["text"] = "Patched title";
        node["layout"]["x"] = 84;
        node["appearance"]["opacity"] = 0.7;
    }
    const auto patchedScene = px::sdk::ParseUi(patchedSceneJson.dump());
    Check(patchedScene.Valid(), "property patch fixture must remain a valid UI document document");
    auto* applicationRootBeforePatch = applicationContext.Root();
    const auto propertyPatch = application.PatchDocumentProperties(patchedScene.document, { .sourcePath = "Content/UI/Title.pxui", .resolveAsset = [](const std::string_view id) -> std::optional<std::string> {
                                                                                               return id == "66666666-6666-4666-8666-666666666666" ? std::optional<std::string>{ "Content/logo.png" } : std::nullopt;
                                                                                           } });
    Check(
        static_cast<bool>(propertyPatch) && applicationContext.Root() == applicationRootBeforePatch && applicationContext.Root()->Find(*buttonId) == applicationButton && applicationButton->Text() == "Patched title" &&
            applicationButton->Offsets().x == 84.0f && applicationButton->Opacity() == 0.7f,
        "shared property patch must preserve Runtime identity and update authored values"
    );

    auto rejectedPatchJson = patchedSceneJson;
    rejectedPatchJson["revision"] = 11;
    for (auto& node : rejectedPatchJson["nodes"]) {
        if (node["id"] == buttonId->ToString())
            node["text"] = "MUST NOT COMMIT";
        if (node["kind"] == "image")
            node["id"] = "abababab-abab-4bab-8bab-abababababab";
    }
    const auto rejectedPatchDocument = px::sdk::ParseUi(rejectedPatchJson.dump());
    Check(rejectedPatchDocument.Valid(),
          "transaction rejection fixture remains a valid detached UI document");
    const auto rejectedPatch = application.PatchDocumentProperties(
        rejectedPatchDocument.document,
        {.sourcePath = "Content/UI/Title.pxui",
         .resolveAsset = [](const std::string_view id) -> std::optional<std::string> {
             return id == "66666666-6666-4666-8666-666666666666"
                        ? std::optional<std::string>{"Content/logo.png"}
                        : std::nullopt;
         }});
    Check(!rejectedPatch && applicationButton->Text() == "Patched title" &&
              applicationButton->Offsets().x == 84.0f &&
              applicationButton->Opacity() == 0.7f,
          "failed detached property patch leaves every live Runtime property unchanged");

    px::ui::GalgameUI playerUi;
    playerUi.SetUiAssetResolver([](const std::string_view id) -> std::optional<std::string> { return id == "66666666-6666-4666-8666-666666666666" ? std::optional<std::string>{ "Content/logo.png" } : std::nullopt; });
    Check(playerUi.RegisterTemplate(px::ui::GalgameUI::Screen::Title, scene, "Content/UI/Title.pxui").IsOk(), "Player UI entry accepts a packaged UI document template");
    const auto titleImages = playerUi.RouteImageDependencies("title");
    Check(titleImages && titleImages.Value() ==
              std::vector<std::string>{"Content/logo.png"},
          "registered route exposes static image dependencies before presentation");
    Check(playerUi.ShowTitle().IsOk(), "Player title route installs through the shared UI document application");
    auto phoneScene = nlohmann::json::parse(scene);
    phoneScene["name"] = "Phone";
    phoneScene["nodes"][0]["name"] = "PhoneRoot";
    Check(playerUi.RegisterRouteTemplate("phone", phoneScene.dump(),
                                         "Content/UI/Phone.pxui").IsOk(),
          "Player UI accepts an arbitrary authored route template");
    Check(playerUi.ShowRoute("phone").IsOk() &&
              playerUi.CurrentScreen() == px::ui::GalgameUI::Screen::Custom &&
              playerUi.CurrentRoute() == "phone" && playerUi.Root() &&
              playerUi.Root()->Name() == "PhoneRoot",
          "arbitrary routes install their real authored scene instead of a placeholder");
    const auto phoneCheckpoint = playerUi.CaptureRuntimeState();
    Check(phoneCheckpoint.surfaceId == "phone" && playerUi.ShowTitle().IsOk() &&
              playerUi.RestoreRuntimeState(phoneCheckpoint).IsOk() &&
              playerUi.CurrentRoute() == "phone" && playerUi.Root() &&
              playerUi.Root()->Name() == "PhoneRoot",
          "arbitrary route identity and UI state survive a screen checkpoint restore");
    px::ui::ActionInvocation playerStart;
    playerStart.action = "game.start";
    playerStart.context.sourceScene = "Content/UI/Title.pxui";
    Check(playerUi.Actions().Dispatch(std::move(playerStart)).IsOk(), "Player UI document route uses the production Action dispatcher");
    std::optional<px::ui::GalgameAction> customAction;
    playerUi.SetActionSink([&customAction](const px::ui::GalgameAction& action) {
        customAction = action;
    });
    Check(playerUi.RegisterActionCommand("music.play", "track").IsOk(),
          "game-specific authored commands must register without an engine whitelist");
    px::ui::ActionInvocation playTrack;
    playTrack.action = "music.play";
    playTrack.arguments.emplace("track", px::Variant("Assets/Audio/BGM/BGM01.wav"));
    Check(playerUi.Actions().Dispatch(std::move(playTrack)).IsOk(),
          "game-specific authored command must dispatch through the production Action dispatcher");
    px::Input customActionInput;
    (void)playerUi.Update(customActionInput, 1920, 1080, 0.016f);
    Check(customAction && customAction->command == "music.play" &&
              customAction->argument == "Assets/Audio/BGM/BGM01.wav",
          "game-specific authored command must preserve its configured string argument");

    const auto componentContract = px::sdk::ParseUiComponent(ComponentSource());
    if (!componentContract.Valid())
        for (const auto& diagnostic : componentContract.diagnostics)
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    Check(componentContract.Valid(), "reusable UI component source must satisfy the SDK contract");
    const auto componentScene = px::sdk::ParseUi(ComponentScene());
    Check(componentScene.Valid(), "expanded reusable component instance must satisfy the scene contract");
    px::ui::UIContext componentContext;
    std::size_t componentActions = 0;
    Check(
        componentContext.Commands()
            .Register(
                "game.start",
                [&componentActions](const px::Variant&) {
                    ++componentActions;
                    return px::Status::Ok();
                }
            )
            .IsOk(),
        "component public signal Action route must register"
    );
    px::ui::UiApplication componentApplication(componentContext);
    const auto componentLoader = [](const std::string_view id) -> std::optional<px::ui::UiComponentSource> {
        if (id != "61616161-6161-4161-8161-616161616161") return std::nullopt;
        return px::ui::UiComponentSource{ "Content/UI/ActionCard.pxuicomponent", ComponentSource() };
    };
    const auto componentApplied = componentApplication.ApplyDocument(componentScene.document, { .sourcePath = "Content/UI/ComponentScene.pxui", .loadComponent = componentLoader });
    Check(static_cast<bool>(componentApplied), "shared application must resolve reusable component public APIs");
    Check(componentApplied.Value().nodeCount == 5 && componentApplied.Value().actionBindingCount == 1, "component projection and public Action binding must reach Runtime");
    const auto complexComponentContract = px::sdk::ParseUiComponent(ComplexComponentSource());
    const auto complexComponentScene = px::sdk::ParseUi(ComplexComponentScene());
    Check(complexComponentContract.Valid() && complexComponentScene.Valid(), "complex component Runtime fixtures must satisfy SDK contracts");
    px::ui::UIContext complexComponentContext;
    px::ui::UiApplication complexComponentApplication(complexComponentContext);
    const auto complexComponentApplied =
        complexComponentApplication.ApplyDocument(complexComponentScene.document, { .sourcePath = "Content/UI/ComplexComponentScene.pxui", .loadComponent = [](const std::string_view id) -> std::optional<px::ui::UiComponentSource> {
                                                                                       if (id != "61616161-6161-4161-8161-616161616161") return std::nullopt;
                                                                                       return px::ui::UiComponentSource{ "Content/UI/ComplexActionCard.pxuicomponent", ComplexComponentSource() };
                                                                                   } });
    Check(
        !complexComponentApplied && HasDiagnostic(complexComponentApplied, "PXUISTUDIO2004") && !HasDiagnostic(complexComponentApplied, "PXUISTUDIO2116"),
        "complex component values must reach the real TypeRegistry boundary instead of an unsupported-value shortcut"
    );
    auto nonSlotDocument = componentScene.document;
    nonSlotDocument.nodes.back().componentSlot.reset();
    px::ui::UIContext nonSlotContext;
    px::ui::UiApplication nonSlotApplication(nonSlotContext);
    const auto nonSlotApplied = nonSlotApplication.ApplyDocument(nonSlotDocument, { .sourcePath = "Content/UI/NonSlotStructure.pxui", .loadComponent = componentLoader });
    Check(!nonSlotApplied && HasDiagnostic(nonSlotApplied, "PXUISTUDIO2127"), "programmatic Runtime application must reject non-slot local component structure");
    const auto componentButtonId = px::Uuid::Parse("63636363-7373-4373-8373-636363636363");
    const auto slotContentId = px::Uuid::Parse("65656565-7575-4575-8575-656565656565");
    Check(componentButtonId && slotContentId, "component Runtime fixture identities must be UUIDs");
    auto* componentButton = dynamic_cast<px::ui::Button*>(componentContext.Root()->Find(*componentButtonId));
    Check(componentButton && componentButton->Text() == "Continue", "typed public property must update the exposed Runtime target");
    Check(componentContext.Root()->Find(*slotContentId) != nullptr, "named slot content must retain its stable Runtime identity");
    componentButton->Activate();
    Check(componentActions == 1, "Button activated public signal must dispatch its typed Action");

    const auto parameterizedComponent = px::sdk::ParseUiComponent(ParameterizedComponentSource());
    const auto parameterizedScene = px::sdk::ParseUi(ParameterizedComponentScene());
    Check(parameterizedComponent.Valid() && parameterizedScene.Valid(), "revision-2 OptionButton signal fixtures must satisfy SDK contracts");
    auto revisionOneSignalMapping = nlohmann::json::parse(ComponentScene());
    revisionOneSignalMapping["schemaRevision"] = 1;
    revisionOneSignalMapping["nodes"][1]["componentInstance"]["publicSignals"]["accepted"]["argumentBindings"] = { { "index", "index" } };
    Check(!px::sdk::ParseUi(revisionOneSignalMapping.dump()).Valid(), "legacy revision-1 UI must be rejected before component signal registration");
    px::ui::UIContext parameterizedContext;
    std::int64_t selectedIndex = -1;
    Check(
        parameterizedContext.Commands()
            .Register(
                "choice.select",
                [&selectedIndex](const px::Variant& value) {
                    const auto* arguments = value.AsObject();
                    const auto found = arguments ? arguments->find("index") : px::VariantObject::const_iterator{};
                    const auto* index = arguments && found != arguments->end() ? found->second.TryGet<std::int64_t>() : nullptr;
                    if (!index) return px::Status::Fail(px::diag::Diagnostic{ .severity = px::diag::Severity::Error, .code = "PXTEST-SIGNAL", .category = "Test", .message = "choice.select index is missing" });
                    selectedIndex = *index;
                    return px::Status::Ok();
                }
            )
            .IsOk(),
        "parameterized component Action route must register"
    );
    px::ui::UiApplication parameterizedApplication(parameterizedContext);
    const auto parameterizedApplied =
        parameterizedApplication.ApplyDocument(parameterizedScene.document, { .sourcePath = "Content/UI/ParameterizedComponentScene.pxui", .loadComponent = [](const std::string_view id) -> std::optional<px::ui::UiComponentSource> {
                                                                                 if (id != "61616161-6161-4161-8161-616161616161") return std::nullopt;
                                                                                 return px::ui::UiComponentSource{ "Content/UI/ParameterizedActionCard.pxuicomponent", ParameterizedComponentSource() };
                                                                             } });
    Check(static_cast<bool>(parameterizedApplied) && parameterizedApplied.Value().actionBindingCount == 1, "shared application must connect one non-Button parameterized signal");
    auto* parameterizedOption = dynamic_cast<px::ui::OptionButton*>(parameterizedContext.Root()->Find(*componentButtonId));
    Check(parameterizedOption != nullptr, "component projection must retain exact OptionButton Runtime identity");
    px::ui::UIEvent selection{ .type = px::ui::UIEventType::Activate };
    parameterizedOption->HandleEvent(selection);
    Check(selectedIndex == 1 && parameterizedOption->Text() == "Finale", "itemSelected(index,text) must map the live typed index into the Action");

    const auto nestedSlotScene = px::sdk::ParseUi(NestedComponentSlotScene());
    if (!nestedSlotScene.Valid())
        for (const auto& diagnostic : nestedSlotScene.diagnostics) std::cerr << diagnostic.code << "[" << diagnostic.nodeIndex << "]: " << diagnostic.message << '\n';
    Check(nestedSlotScene.Valid(), "nested component roots may be projected as named slot content");
    px::ui::UIContext nestedSlotContext;
    px::ui::UiApplication nestedSlotApplication(nestedSlotContext);
    const auto nestedSlotLoader = [](const std::string_view id) -> std::optional<px::ui::UiComponentSource> {
        if (id == "71717171-7171-4171-8171-717171717171") return px::ui::UiComponentSource{ "Content/UI/NestedShell.pxuicomponent", NestedComponentSource() };
        if (id == "61616161-6161-4161-8161-616161616161") return px::ui::UiComponentSource{ "Content/UI/ActionCard.pxuicomponent", ComponentSource() };
        return std::nullopt;
    };
    const auto nestedSlotApplied = nestedSlotApplication.ApplyDocument(nestedSlotScene.document, { .sourcePath = "Content/UI/NestedSlotScene.pxui", .loadComponent = nestedSlotLoader });
    Check(static_cast<bool>(nestedSlotApplied) && nestedSlotApplied.Value().nodeCount == 9, "shared application must install a nested component root into slot content");
    const auto nestedSlotRootId = px::Uuid::Parse("76767676-7676-4767-8767-767676767676");
    Check(nestedSlotRootId && nestedSlotContext.Root()->Find(*nestedSlotRootId) != nullptr, "nested component slot root must retain its Runtime identity");

    auto malformedSlotJson = nlohmann::json::parse(NestedComponentSlotScene());
    malformedSlotJson["nodes"][6]["componentSlot"] = { { "instanceRootId", "62626262-7272-4272-8272-626262626262" }, { "slotId", "content" } };
    px::ui::UIContext malformedSlotContext;
    px::ui::UiApplication malformedSlotApplication(malformedSlotContext);
    const auto malformedSlot = malformedSlotApplication.ApplyText(malformedSlotJson.dump(), { .sourcePath = "Content/UI/MalformedNestedSlotScene.pxui", .loadComponent = nestedSlotLoader });
    Check(!malformedSlot && HasDiagnostic(malformedSlot, "PXSDKUI1038"), "malformed non-root nested component slot placement must fail closed");

    px::ui::UIContext missingLoaderContext;
    px::ui::UiApplication missingLoaderApplication(missingLoaderContext);
    Check(!missingLoaderApplication.ApplyDocument(componentScene.document, { .sourcePath = "missing-loader.pxui" }), "component instances must fail closed without a production loader");
    const auto wrongType = px::sdk::ParseUi(ComponentScene(false, true));
    Check(wrongType.Valid(), "wrong-type fixture remains a valid wire document");
    px::ui::UIContext wrongTypeContext;
    px::ui::UiApplication wrongTypeApplication(wrongTypeContext);
    Check(!wrongTypeApplication.ApplyDocument(wrongType.document, { .sourcePath = "wrong-type.pxui", .loadComponent = componentLoader }), "public property type mismatch must fail before Runtime installation");
    const auto wrongSlot = px::sdk::ParseUi(ComponentScene(true, false));
    Check(wrongSlot.Valid(), "wrong-slot fixture remains structurally valid");
    px::ui::UIContext wrongSlotContext;
    px::ui::UiApplication wrongSlotApplication(wrongSlotContext);
    Check(!wrongSlotApplication.ApplyDocument(wrongSlot.document, { .sourcePath = "wrong-slot.pxui", .loadComponent = componentLoader }), "named slot content must fail when attached outside its declared target");

    const std::string recursiveComponent = DependencyComponent("61616161-6161-4161-8161-616161616161", "61616161-6161-4161-8161-616161616161");
    px::ui::UIContext recursiveContext;
    px::ui::UiApplication recursiveApplication(recursiveContext);
    const auto recursiveApplied = recursiveApplication.ApplyDocument(componentScene.document, { .sourcePath = "recursive.pxui", .loadComponent = [&recursiveComponent](const std::string_view id) -> std::optional<px::ui::UiComponentSource> {
                                                                                                   return id == "61616161-6161-4161-8161-616161616161"
                                                                                                              ? std::optional<px::ui::UiComponentSource>{ px::ui::UiComponentSource{ "recursive.pxuicomponent", recursiveComponent } }
                                                                                                              : std::nullopt;
                                                                                               } });
    Check(!recursiveApplied && HasDiagnostic(recursiveApplied, "PXUISTUDIO2113"), "recursive component dependency must fail with a cycle diagnostic");

    px::ui::GalgameUI componentPlayerUi;
    componentPlayerUi.SetUiComponentLoader(componentLoader);
    Check(componentPlayerUi.RegisterTemplate(px::ui::GalgameUI::Screen::Title, ComponentScene(), "Content/UI/ComponentScene.pxui").IsOk() && componentPlayerUi.ShowTitle().IsOk(), "Player UI must use the shared reusable component resolver");

    const auto nestedSource = px::sdk::ParseUiComponent(NestedComponentSource());
    const auto nestedScene = px::sdk::ParseUi(NestedComponentScene());
    Check(nestedSource.Valid() && nestedScene.Valid(), "nested component source and expanded scene contracts must parse");
    px::ui::UIContext nestedContext;
    std::size_t nestedActions = 0;
    Check(
        nestedContext.Commands()
            .Register(
                "game.start",
                [&nestedActions](const px::Variant&) {
                    ++nestedActions;
                    return px::Status::Ok();
                }
            )
            .IsOk(),
        "nested component Action route must register"
    );
    px::ui::UiApplication nestedApplication(nestedContext);
    const auto nestedApplied = nestedApplication.ApplyDocument(nestedScene.document, { .sourcePath = "Content/UI/NestedComponentScene.pxui", .loadComponent = [](const std::string_view id) -> std::optional<px::ui::UiComponentSource> {
                                                                                          if (id == "71717171-7171-4171-8171-717171717171") return px::ui::UiComponentSource{ "Content/UI/NestedShell.pxuicomponent", NestedComponentSource() };
                                                                                          if (id == "61616161-6161-4161-8161-616161616161") return px::ui::UiComponentSource{ "Content/UI/ActionCard.pxuicomponent", ComponentSource() };
                                                                                          return std::nullopt;
                                                                                      } });
    Check(static_cast<bool>(nestedApplied) && nestedApplied.Value().nodeCount == 5 && nestedApplied.Value().actionBindingCount == 1, "bounded nested component projection must install through the shared Runtime");
    const auto nestedButtonId = px::Uuid::Parse("84848484-8484-4484-8484-848484848484");
    Check(nestedButtonId.has_value(), "nested Runtime button identity must parse");
    auto* nestedButton = dynamic_cast<px::ui::Button*>(nestedContext.Root()->Find(*nestedButtonId));
    Check(nestedButton && nestedButton->Text() == "Nested continue", "nested public property must apply without regenerating Runtime identity");
    nestedButton->Activate();
    Check(nestedActions == 1, "nested public signal must dispatch through the shared typed Action path");
    px::ui::GalgameUI nestedPlayerUi;
    nestedPlayerUi.SetUiComponentLoader([](const std::string_view id) -> std::optional<px::ui::UiComponentSource> {
        if (id == "71717171-7171-4171-8171-717171717171") return px::ui::UiComponentSource{ "Content/UI/NestedShell.pxuicomponent", NestedComponentSource() };
        if (id == "61616161-6161-4161-8161-616161616161") return px::ui::UiComponentSource{ "Content/UI/ActionCard.pxuicomponent", ComponentSource() };
        return std::nullopt;
    });
    Check(
        nestedPlayerUi.RegisterTemplate(px::ui::GalgameUI::Screen::Title, NestedComponentScene(), "Content/UI/NestedComponentScene.pxui").IsOk() && nestedPlayerUi.ShowTitle().IsOk(),
        "Player must install nested components through the shared application semantics"
    );

    px::ui::UIContext missingNestedContext;
    px::ui::UiApplication missingNestedApplication(missingNestedContext);
    const auto missingNested = missingNestedApplication.ApplyDocument(nestedScene.document, { .sourcePath = "missing-nested.pxui", .loadComponent = [](const std::string_view id) -> std::optional<px::ui::UiComponentSource> {
                                                                                                 return id == "71717171-7171-4171-8171-717171717171"
                                                                                                            ? std::optional<px::ui::UiComponentSource>{ px::ui::UiComponentSource{ "NestedShell.pxuicomponent", NestedComponentSource() } }
                                                                                                            : std::nullopt;
                                                                                             } });
    Check(!missingNested && HasDiagnostic(missingNested, "PXUISTUDIO2111"), "missing nested UUID source must produce an explicit diagnostic");

    nlohmann::json stalePathJson = nlohmann::json::parse(NestedComponentScene());
    stalePathJson["nodes"][3]["componentInstance"]["sourcePath"][3] = "62626262-6262-4262-8262-626262626262";
    const auto stalePathScene = px::sdk::ParseUi(stalePathJson.dump());
    Check(stalePathScene.Valid(), "stale nested path fixture must remain a valid wire document");
    px::ui::UIContext stalePathContext;
    px::ui::UiApplication stalePathApplication(stalePathContext);
    const auto stalePath = stalePathApplication.ApplyDocument(stalePathScene.document, { .sourcePath = "stale-nested-path.pxui", .loadComponent = [](const std::string_view id) -> std::optional<px::ui::UiComponentSource> {
                                                                                            if (id == "71717171-7171-4171-8171-717171717171") return px::ui::UiComponentSource{ "NestedShell.pxuicomponent", NestedComponentSource() };
                                                                                            if (id == "61616161-6161-4161-8161-616161616161") return px::ui::UiComponentSource{ "ActionCard.pxuicomponent", ComponentSource() };
                                                                                            return std::nullopt;
                                                                                        } });
    Check(!stalePath && HasDiagnostic(stalePath, "PXUISTUDIO2126"), "stale nested UUID sourcePath must fail before Runtime installation");

    std::vector<std::string> dependencyIds;
    for (std::size_t index = 0; index < 33; ++index) dependencyIds.push_back(BudgetComponentId(index));
    px::ui::UIContext depthContext;
    px::ui::UiApplication depthApplication(depthContext);
    const auto depthExceeded = depthApplication.ApplyDocument(componentScene.document, { .sourcePath = "depth-budget.pxui", .loadComponent = [&dependencyIds](const std::string_view id) -> std::optional<px::ui::UiComponentSource> {
                                                                                            const auto found = std::ranges::find(dependencyIds, id);
                                                                                            if (found == dependencyIds.end()) return std::nullopt;
                                                                                            const auto index = static_cast<std::size_t>(std::distance(dependencyIds.begin(), found));
                                                                                            const std::optional<std::string> next = index + 1 < dependencyIds.size() ? std::optional<std::string>{ dependencyIds[index + 1] } : std::nullopt;
                                                                                            return px::ui::UiComponentSource{ "depth-" + std::to_string(index) + ".pxuicomponent", DependencyComponent(dependencyIds[index], next) };
                                                                                        } });
    Check(!depthExceeded && HasDiagnostic(depthExceeded, "PXUISTUDIO2122"), "nested component depth budget must fail closed");

    px::ui::UIContext byteContext;
    px::ui::UiApplication byteApplication(byteContext);
    const auto byteExceeded = byteApplication.ApplyDocument(componentScene.document, { .sourcePath = "byte-budget.pxui", .loadComponent = [](const std::string_view) -> std::optional<px::ui::UiComponentSource> {
                                                                                          return px::ui::UiComponentSource{ "oversized.pxuicomponent", std::string(8 * 1024 * 1024 + 1, ' ') };
                                                                                      } });
    Check(!byteExceeded && HasDiagnostic(byteExceeded, "PXUISTUDIO2125"), "component source byte budget must fail before JSON parsing");

    px::sdk::UiDocument nodeBudgetDocument;
    nodeBudgetDocument.nodes.resize(65'537);
    nodeBudgetDocument.nodes.front().componentInstance =
        px::sdk::UiComponentInstance{ .componentId = "61616161-6161-4161-8161-616161616161", .instanceRootId = "62626262-7272-4272-8272-626262626262", .sourceNodeId = "62626262-6262-4262-8262-626262626262" };
    px::ui::UIContext nodeContext;
    px::ui::UiApplication nodeApplication(nodeContext);
    const auto nodeExceeded = nodeApplication.ApplyDocument(nodeBudgetDocument, { .sourcePath = "node-budget.pxui", .loadComponent = componentLoader });
    Check(!nodeExceeded && HasDiagnostic(nodeExceeded, "PXUISTUDIO2124"), "component projection node budget must fail before Runtime construction");
    return 0;
}
