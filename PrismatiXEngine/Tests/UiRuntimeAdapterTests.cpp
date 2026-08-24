#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

#include "Engine/SDK/Ui.h"
#include "Engine/UI/GalgameUI.h"
#include "Engine/UI/UiAdapter.h"
#include "Engine/UI/UiApplication.h"
#include "Engine/UI/UIContext.h"
#include "Engine/UI/UITypeRegistry.h"
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

std::string ComponentSource() {
    return R"({
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
    })";
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
    return std::string{
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
    })";
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

int main() {
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
    const auto parsed = px::sdk::ParseUi(scene);
    Check(parsed.Valid(), "valid UI document must parse before Runtime adaptation");
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
    optionScene["theme"].push_back({ { "id", "19191919-1919-4919-8919-191919191919" }, { "name", "opacity_soft" }, { "value", "0.35" } });
    optionScene["theme"].push_back({ { "id", "20202020-2020-4020-8020-202020202020" }, { "name", "tint_soft" }, { "value", "#336699" } });
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

    auto patchedSceneJson = nlohmann::json::parse(scene);
    patchedSceneJson["revision"] = 10;
    for (auto& node : patchedSceneJson["nodes"]) {
        if (node["id"] != buttonId->ToString()) continue;
        node["content"]["text"] = "Patched title";
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

    px::ui::GalgameUI playerUi;
    playerUi.SetUiAssetResolver([](const std::string_view id) -> std::optional<std::string> { return id == "66666666-6666-4666-8666-666666666666" ? std::optional<std::string>{ "Content/logo.png" } : std::nullopt; });
    Check(playerUi.RegisterTemplate(px::ui::GalgameUI::Screen::Title, scene, "Content/UI/Title.pxui").IsOk(), "Player UI entry accepts a packaged UI document template");
    Check(playerUi.ShowTitle().IsOk(), "Player title route installs through the shared UI document application");
    px::ui::ActionInvocation playerStart;
    playerStart.action = "game.start";
    playerStart.context.sourceScene = "Content/UI/Title.pxui";
    Check(playerUi.Actions().Dispatch(std::move(playerStart)).IsOk(), "Player UI document route uses the production Action dispatcher");

    const auto componentContract = px::sdk::ParseUiComponent(ComponentSource());
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
    revisionOneSignalMapping["nodes"][1]["componentInstance"]["publicSignals"]["accepted"]["argumentBindings"] = { { "index", "index" } };
    Check(!px::sdk::ParseUi(revisionOneSignalMapping.dump()).Valid(), "revision-1 component signal bindings must reject parameter mappings");
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
