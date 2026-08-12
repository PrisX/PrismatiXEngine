#include "Engine/SDK/StudioUi.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void Check(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

std::string ValidScene() {
    return R"({
      "format":"PrismatiXUIScene","schemaRevision":1,
      "id":"11111111-1111-4111-8111-111111111111","revision":4,
      "name":"Title","width":1280,"height":720,
      "rootId":"22222222-2222-4222-8222-222222222222",
      "nodes":[
        {"id":"22222222-2222-4222-8222-222222222222","parentId":null,"order":0,
         "kind":"control","name":"Root","visible":true,"locked":false,
         "layout":{"mode":"free","x":0,"y":0,"width":1280,"height":720,
                   "anchorX":0,"anchorY":0,"pivotX":0,"pivotY":0,"margin":0,
                   "alignment":"start","sizeRule":"fixed"},
         "content":{"text":"","assetId":null},
         "appearance":{"backgroundColor":"#16121A","textColor":"#F5EEF6",
                       "opacity":1,"styleToken":null,"hoverBackgroundColor":null,
                       "focusColor":null,"disabledOpacity":0.5},
         "interaction":{"onClick":null},
         "accessibility":{"label":"","role":"presentation"}},
        {"id":"33333333-3333-4333-8333-333333333333",
         "parentId":"22222222-2222-4222-8222-222222222222","order":0,
         "kind":"button","name":"Start","visible":true,"locked":false,
         "layout":{"mode":"free","x":500,"y":560,"width":280,"height":64,
                   "anchorX":0.5,"anchorY":0.5,"pivotX":0.5,"pivotY":0.5,"margin":0,
                   "alignment":"center","sizeRule":"fixed"},
         "content":{"text":"開始","assetId":null},
         "appearance":{"backgroundColor":"#F052A0","textColor":"#FFFFFF",
                       "opacity":1,"styleToken":"accent","hoverBackgroundColor":"#FF79BA",
                       "focusColor":"#FFFFFF","disabledOpacity":0.5},
         "interaction":{"onClick":{"id":"game.start","arguments":{}}},
         "accessibility":{"label":"Start game","role":"button"},
         "componentInstance":{
           "componentId":"55555555-5555-4555-8555-555555555555",
           "instanceRootId":"33333333-3333-4333-8333-333333333333",
           "sourceNodeId":"66666666-6666-4666-8666-666666666666",
           "overrides":["content.text","accessibility.label"]}}
      ],
      "theme":[{"id":"44444444-4444-4444-8444-444444444444",
                 "name":"accent","value":"#F052A0"}]
    })";
}

std::string CombinedComponentSlotScene(const bool nonRootSlot) {
    std::string scene = ValidScene();
    const auto insertion = scene.find("],");
    Check(insertion != std::string::npos,
          "combined component slot fixture must be editable");
    const std::string rootSlot =
        nonRootSlot
            ? ""
            : R"(,"componentSlot":{"instanceRootId":"33333333-3333-4333-8333-333333333333","slotId":"content"})";
    const std::string childSlot =
        nonRootSlot
            ? R"(,"componentSlot":{"instanceRootId":"33333333-3333-4333-8333-333333333333","slotId":"content"})"
            : "";
    scene.insert(
        insertion,
        R"(,
         {"id":"77777777-7777-4777-8777-777777777777",
          "parentId":"33333333-3333-4333-8333-333333333333","order":0,
          "kind":"group","name":"NestedRoot","visible":true,"locked":false,
          "layout":{"mode":"free","x":0,"y":0,"width":120,"height":48,
                    "anchorX":0,"anchorY":0,"pivotX":0,"pivotY":0,"margin":0,
                    "alignment":"start","sizeRule":"fixed"},
          "content":{"text":"","assetId":null},
          "appearance":{"backgroundColor":"#00000000","textColor":"#FFFFFF",
                        "opacity":1,"styleToken":null,"hoverBackgroundColor":null,
                        "focusColor":null,"disabledOpacity":0.5},
          "interaction":{"onClick":null},
          "accessibility":{"label":"Nested root","role":"group"},
          "componentInstance":{
            "componentId":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
            "instanceRootId":"77777777-7777-4777-8777-777777777777",
            "sourceNodeId":"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
            "sourcePath":["aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa","bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"],
            "overrides":[]})" + rootSlot + R"(},
         {"id":"88888888-8888-4888-8888-888888888888",
          "parentId":"77777777-7777-4777-8777-777777777777","order":0,
          "kind":"label","name":"NestedChild","visible":true,"locked":false,
          "layout":{"mode":"free","x":0,"y":0,"width":120,"height":24,
                    "anchorX":0,"anchorY":0,"pivotX":0,"pivotY":0,"margin":0,
                    "alignment":"start","sizeRule":"fixed"},
          "content":{"text":"Child","assetId":null},
          "appearance":{"backgroundColor":"#00000000","textColor":"#FFFFFF",
                        "opacity":1,"styleToken":null,"hoverBackgroundColor":null,
                        "focusColor":null,"disabledOpacity":0.5},
          "interaction":{"onClick":null},
          "accessibility":{"label":"Nested child","role":"text"},
          "componentInstance":{
            "componentId":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
            "instanceRootId":"77777777-7777-4777-8777-777777777777",
            "sourceNodeId":"cccccccc-cccc-4ccc-8ccc-cccccccccccc",
            "sourcePath":["aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa","cccccccc-cccc-4ccc-8ccc-cccccccccccc"],
            "overrides":[]})" + childSlot + R"(}
       )");
    return scene;
}

std::string LocalComponentContentScene(const bool namedSlot) {
    std::string scene = ValidScene();
    const auto insertion = scene.find("],");
    Check(insertion != std::string::npos,
          "local component content fixture must be editable");
    const std::string slot = namedSlot
                                 ? R"(,"componentSlot":{"instanceRootId":"33333333-3333-4333-8333-333333333333","slotId":"content"})"
                                 : "";
    scene.insert(
        insertion,
        R"(,
         {"id":"77777777-7777-4777-8777-777777777777",
          "parentId":"33333333-3333-4333-8333-333333333333","order":0,
          "kind":"label","name":"LocalContent","visible":true,"locked":false,
          "layout":{"mode":"free","x":0,"y":0,"width":120,"height":24,
                    "anchorX":0,"anchorY":0,"pivotX":0,"pivotY":0,"margin":0,
                    "alignment":"start","sizeRule":"fixed"},
          "content":{"text":"Local","assetId":null},
          "appearance":{"backgroundColor":"#00000000","textColor":"#FFFFFF",
                        "opacity":1,"styleToken":null,"hoverBackgroundColor":null,
                        "focusColor":null,"disabledOpacity":0.5},
          "interaction":{"onClick":null},
          "accessibility":{"label":"Local content","role":"text"})" +
            slot + R"(}
        )");
    return scene;
}

std::string AdvancedScene() {
    std::string scene = ValidScene();
    const auto end = scene.rfind("\n    }");
    Check(end != std::string::npos, "base Studio UI fixture must have an object end");
    scene.insert(end, R"(,
      "behaviorGraph":{
        "nodes":[
          {"id":"77777777-7777-4777-8777-777777777777","kind":"signalEntry",
           "position":{"x":80,"y":120},"properties":{},"arguments":{}},
          {"id":"88888888-8888-4888-8888-888888888888","kind":"setProperty",
           "position":{"x":360,"y":120},
           "properties":{"target":{"type":"nodeReference","nodeId":"33333333-3333-4333-8333-333333333333"},"property":"opacity","value":0.25},
           "arguments":{}}
        ],
        "links":[
          {"id":"99999999-9999-4999-8999-999999999999",
           "fromNodeId":"77777777-7777-4777-8777-777777777777","fromPin":"out",
           "toNodeId":"88888888-8888-4888-8888-888888888888","toPin":"in"}
        ],
        "groups":[]
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
    return scene;
}

std::string BehaviorInspectorScene(const std::string_view kind,
                                   const std::string_view properties) {
    std::string scene = ValidScene();
    const auto end = scene.rfind("\n    }");
    Check(end != std::string::npos,
          "Behavior inspector fixture must have an object end");
    scene.insert(
        end,
        ",\n      \"behaviorGraph\":{\"nodes\":[{\"id\":\"77777777-7777-4777-8777-777777777777\",\"kind\":\"" +
            std::string(kind) +
            "\",\"position\":{\"x\":80,\"y\":120},\"properties\":" +
            std::string(properties) +
            ",\"arguments\":{}}],\"links\":[],\"groups\":[]},\n"
            "      \"behaviorTriggers\":[],\n"
            "      \"animations\":{\"clips\":[{\"id\":\"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb\",\"name\":\"Focused clip\",\"duration\":1,\"loop\":false,\"tracks\":[]}],"
            "\"stateMachine\":{\"entryStateId\":\"cccccccc-cccc-4ccc-8ccc-cccccccccccc\","
            "\"parameters\":[{\"id\":\"dddddddd-dddd-4ddd-8ddd-dddddddddddd\",\"name\":\"speed\",\"type\":\"number\",\"defaultValue\":1}],"
            "\"states\":[{\"id\":\"cccccccc-cccc-4ccc-8ccc-cccccccccccc\",\"name\":\"Focused\",\"clipId\":\"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb\",\"position\":{\"x\":80,\"y\":80}}],\"transitions\":[]}}");
    return scene;
}

std::string ComponentContract() {
    std::string component = ValidScene();
    component.replace(component.find("PrismatiXUIScene"),
                      std::string_view("PrismatiXUIScene").size(),
                      "PrismatiXUIComponent");
    const auto end = component.rfind("\n    }");
    Check(end != std::string::npos,
          "component contract fixture must have an object end");
    component.insert(end, R"(,
      "componentInterface":{
        "properties":[{
          "id":"caption","displayName":"Caption",
          "nodeId":"33333333-3333-4333-8333-333333333333",
          "property":"content.text","valueType":"string",
          "defaultValue":"開始","metadata":{"owner":"runtime"}}],
        "signals":[{
          "id":"accepted","displayName":"Accepted",
          "nodeId":"33333333-3333-4333-8333-333333333333",
          "signal":"activated","arguments":[],
          "metadata":{"route":"action"}}],
        "slots":[{
          "id":"content","displayName":"Content",
          "nodeId":"22222222-2222-4222-8222-222222222222",
          "metadata":{"policy":"append"}}],
        "metadata":{"schemaOwner":"Studio"}
      })");
    return component;
}

std::string ComplexComponentContract() {
    using Json = nlohmann::json;
    Json component = Json::parse(ComponentContract());
    auto& node = component["nodes"][0];
    node["runtimeProperties"] = {
        {"owner", "a6c8947c-460f-42ef-a8ea-3a420e80df99"},
        {"resource", {{"type", "resource"},
                      {"value", "a6c8947c-460f-42ef-a8ea-3a420e80df99"}}},
        {"style", {{"type", "token"}, {"value", "accent.primary"}}},
        {"stops", Json::array({0.25, 0.75})},
        {"settings", {{"difficulty", "normal"},
                      {"flags", Json::array({true, false})}}}};
    auto& properties = component["componentInterface"]["properties"];
    properties.push_back({
        {"id", "owner"}, {"displayName", "Owner"}, {"nodeId", node["id"]},
        {"property", "runtimeProperties.owner"}, {"valueType", "uuid"},
        {"defaultValue", node["runtimeProperties"]["owner"]}});
    properties.push_back({
        {"id", "resource"}, {"displayName", "Resource"}, {"nodeId", node["id"]},
        {"property", "runtimeProperties.resource"}, {"valueType", "resource"},
        {"defaultValue", node["runtimeProperties"]["resource"]}});
    properties.push_back({
        {"id", "style"}, {"displayName", "Style"}, {"nodeId", node["id"]},
        {"property", "runtimeProperties.style"}, {"valueType", "token"},
        {"defaultValue", node["runtimeProperties"]["style"]}});
    properties.push_back({
        {"id", "stops"}, {"displayName", "Stops"}, {"nodeId", node["id"]},
        {"property", "runtimeProperties.stops"}, {"valueType", "array"},
        {"defaultValue", node["runtimeProperties"]["stops"]}});
    properties.push_back({
        {"id", "settings"}, {"displayName", "Settings"}, {"nodeId", node["id"]},
        {"property", "runtimeProperties.settings"}, {"valueType", "object"},
        {"defaultValue", node["runtimeProperties"]["settings"]}});
    return component.dump();
}

}  // namespace

int main() {
    const auto parsed = px::sdk::ParseStudioUi(ValidScene());
    Check(parsed.Valid(), "valid named Studio UI must parse");
    Check(parsed.document.revision == 4, "revision must round-trip");
    Check(parsed.document.nodes.size() == 2, "node count must round-trip");
    Check(parsed.document.nodes[1].onClick.has_value(), "typed action must parse");
    Check(parsed.document.nodes[1].onClick->id == "game.start",
          "typed action identity must round-trip");
    Check(parsed.document.nodes[1].componentInstance.has_value() &&
              parsed.document.nodes[1].componentInstance->overrides.size() == 2,
          "component instance origin and overrides must round-trip");

    auto dynamicSceneJson = nlohmann::json::parse(ValidScene());
    dynamicSceneJson["schemaRevision"] = 2;
    dynamicSceneJson["nodes"][1]["kind"] = "leaf";
    dynamicSceneJson["nodes"][1]["runtimeType"] = "OptionButton";
    dynamicSceneJson["nodes"][1]["interaction"]["onClick"] = nullptr;
    dynamicSceneJson["nodes"][1]["runtimeProperties"] = {
        {"options", nlohmann::json::array({"Start", "Load", "Quit"})},
        {"texture", {{"type", "resource"},
                     {"value", "a6c8947c-460f-42ef-a8ea-3a420e80df99"}}},
        {"styleToken", {{"type", "token"}, {"value", "accent"}}}};
    dynamicSceneJson["nodes"][1]["bindings"] = {
        {"text", {{"path", "dialogue.text"}}}};
    const auto dynamicScene =
        px::sdk::ParseStudioUi(dynamicSceneJson.dump());
    Check(dynamicScene.Valid() &&
              dynamicScene.document.schemaRevision == 2 &&
              dynamicScene.document.nodes[1].kind ==
                  px::sdk::StudioUiNodeKind::Leaf &&
              dynamicScene.document.nodes[1].runtimeType == "OptionButton" &&
              std::holds_alternative<px::sdk::StudioUiArrayValue>(
                  dynamicScene.document.nodes[1].runtimeProperties.at(
                      "options")) &&
              std::holds_alternative<px::sdk::StudioUiResourceValue>(
                  dynamicScene.document.nodes[1].runtimeProperties.at(
                      "texture")) &&
              std::holds_alternative<px::sdk::StudioUiTokenValue>(
                  dynamicScene.document.nodes[1].runtimeProperties.at(
                      "styleToken")) &&
              dynamicScene.document.nodes[1].bindings.at("text").path ==
                  "dialogue.text",
          "revision-2 Runtime identity, semantic properties and bindings must parse");
    auto missingRuntimeType = dynamicSceneJson;
    missingRuntimeType["nodes"][1].erase("runtimeType");
    Check(!px::sdk::ParseStudioUi(missingRuntimeType.dump()).Valid(),
          "revision-2 leaf nodes must require runtimeType");
    auto revisionOneRuntimeType = nlohmann::json::parse(ValidScene());
    revisionOneRuntimeType["nodes"][1]["runtimeType"] = "OptionButton";
    Check(!px::sdk::ParseStudioUi(revisionOneRuntimeType.dump()).Valid(),
          "revision-1 nodes must reject runtimeType instead of mis-instantiating");
    auto revisionOneBinding = nlohmann::json::parse(ValidScene());
    revisionOneBinding["nodes"][1]["bindings"] = {
        {"text", {{"path", "dialogue.text"}}}};
    Check(!px::sdk::ParseStudioUi(revisionOneBinding.dump()).Valid(),
          "revision-1 nodes must reject property bindings");
    auto filesystemResource = dynamicSceneJson;
    filesystemResource["nodes"][1]["runtimeProperties"]["texture"]["value"] =
        "Content/title.png";
    Check(!px::sdk::ParseStudioUi(filesystemResource.dump()).Valid(),
          "ResourceRef Runtime values must reject filesystem paths");
    auto emptyRuntimeToken = dynamicSceneJson;
    emptyRuntimeToken["nodes"][1]["runtimeProperties"]["styleToken"]["value"] = "";
    Check(!px::sdk::ParseStudioUi(emptyRuntimeToken.dump()).Valid(),
          "TokenRef Runtime values must reject empty identities");

    const auto complexScene = [] {
        nlohmann::json scene = nlohmann::json::parse(ValidScene());
        scene["nodes"][0]["runtimeProperties"] = {
            {"stops", nlohmann::json::array({0.25, 0.75})},
            {"settings", {{"difficulty", "normal"},
                          {"flags", nlohmann::json::array({true})}}}};
        return px::sdk::ParseStudioUi(scene.dump());
    }();
    Check(complexScene.Valid(),
          "bounded array/object Runtime properties must cross the SDK contract");
    const auto& complexProperties = complexScene.document.nodes[0].runtimeProperties;
    Check(std::holds_alternative<px::sdk::StudioUiArrayValue>(
              complexProperties.at("stops")) &&
              std::holds_alternative<px::sdk::StudioUiObjectValue>(
                  complexProperties.at("settings")),
          "SDK must retain array/object Runtime values as distinct typed payloads");
    auto oversizedRuntimeValue = nlohmann::json::parse(ValidScene());
    oversizedRuntimeValue["nodes"][0]["runtimeProperties"] = {
        {"items", nlohmann::json::array()}};
    oversizedRuntimeValue["nodes"][0]["runtimeProperties"]["items"] =
        std::vector<int>(1025, 1);
    Check(!px::sdk::ParseStudioUi(oversizedRuntimeValue.dump()).Valid(),
          "SDK must reject Runtime collections beyond the shared entry budget");

    auto constrainedScene = nlohmann::json::parse(ValidScene());
    constrainedScene["schemaRevision"] = 2;
    constrainedScene["nodes"][1]["layout"]["anchorRight"] = 0.75;
    constrainedScene["nodes"][1]["layout"]["anchorBottom"] = 1.0;
    const auto constrained = px::sdk::ParseStudioUi(constrainedScene.dump());
    Check(constrained.Valid() &&
              constrained.document.nodes[1].layout.anchorRight == 0.75f &&
              constrained.document.nodes[1].layout.anchorBottom == 1.0f,
          "revision-2 edge constraints must cross the SDK contract");
    constrainedScene["schemaRevision"] = 1;
    Check(!px::sdk::ParseStudioUi(constrainedScene.dump()).Valid(),
          "revision-1 documents must reject edge constraints");
    constrainedScene["schemaRevision"] = 2;
    constrainedScene["nodes"][1]["layout"]["anchorRight"] = 0.25;
    Check(!px::sdk::ParseStudioUi(constrainedScene.dump()).Valid(),
          "constraint edges must not cross their leading anchors");

    const auto combinedRoot =
        px::sdk::ParseStudioUi(CombinedComponentSlotScene(false));
    Check(combinedRoot.Valid(),
          "component instance roots may also carry named slot metadata");
    const auto combinedNonRoot =
        px::sdk::ParseStudioUi(CombinedComponentSlotScene(true));
    Check(!combinedNonRoot.Valid() &&
              std::ranges::any_of(
                  combinedNonRoot.diagnostics,
                  [](const px::sdk::StudioUiContractDiagnostic& diagnostic) {
                      return diagnostic.code == "PXSDKUI1038";
                  }),
          "non-root component projections carrying slot metadata must fail with PXSDKUI1038");

    const auto nonSlotContent =
        px::sdk::ParseStudioUi(LocalComponentContentScene(false));
    Check(!nonSlotContent.Valid() &&
              std::ranges::any_of(
                  nonSlotContent.diagnostics,
                  [](const px::sdk::StudioUiContractDiagnostic& diagnostic) {
                      return diagnostic.code == "PXSDKUI1039";
                  }),
          "local component structure must fail without a named slot");
    Check(px::sdk::ParseStudioUi(LocalComponentContentScene(true)).Valid(),
          "declared named-slot metadata must remain the structural extension seam");

    std::string nestedProjection = ValidScene();
    const std::string nestedAnchor =
        "\"sourceNodeId\":\"66666666-6666-4666-8666-666666666666\",";
    const auto nestedPosition = nestedProjection.find(nestedAnchor);
    Check(nestedPosition != std::string::npos,
          "nested sourcePath fixture must be editable");
    nestedProjection.insert(
        nestedPosition + nestedAnchor.size(),
        R"("sourcePath":["55555555-5555-4555-8555-555555555555","77777777-7777-4777-8777-777777777777","88888888-8888-4888-8888-888888888888","99999999-9999-4999-8999-999999999999"],)");
    const auto nestedParsed = px::sdk::ParseStudioUi(nestedProjection);
    Check(nestedParsed.Valid() &&
              nestedParsed.document.nodes[1]
                      .componentInstance->sourcePath.size() == 4,
          "nested component sourcePath must retain its UUID authority chain");
    std::string malformedNestedProjection = nestedProjection;
    const auto malformedIdentity = malformedNestedProjection.find(
        "88888888-8888-4888-8888-888888888888");
    malformedNestedProjection.replace(malformedIdentity, 36, "not-a-uuid");
    Check(!px::sdk::ParseStudioUi(malformedNestedProjection).Valid(),
          "nested sourcePath must reject non-UUID dependency identities");

    std::string future = ValidScene();
    future.replace(future.find("\"schemaRevision\":1"), 18,
                   "\"schemaRevision\":3");
    Check(!px::sdk::ParseStudioUi(future).Valid(),
          "future Studio UI schema revision must be rejected");

    std::string extensionAction = ValidScene();
    extensionAction.replace(extensionAction.find("game.start"), 10, "fake.start");
    const auto extensionParsed = px::sdk::ParseStudioUi(extensionAction);
    Check(extensionParsed.Valid() &&
              extensionParsed.document.nodes[1].onClick->id == "fake.start",
          "safe namespaced extension Actions must round-trip for runtime catalog resolution");

    std::string malformedAction = ValidScene();
    malformedAction.replace(malformedAction.find("game.start"), 10, "bad action");
    Check(!px::sdk::ParseStudioUi(malformedAction).Valid(),
          "malformed typed Action identities must be rejected");

    std::string invalidBuiltinArguments = ValidScene();
    invalidBuiltinArguments.replace(invalidBuiltinArguments.find("game.start"), 10,
                                    "save.slot");
    Check(!px::sdk::ParseStudioUi(invalidBuiltinArguments).Valid(),
          "known built-in Action argument contracts remain strict");

    std::string unknownOverride = ValidScene();
    unknownOverride.replace(unknownOverride.find("content.text"), 12,
                            "unknown.path");
    Check(!px::sdk::ParseStudioUi(unknownOverride).Valid(),
          "unknown component override paths must be rejected");

    const auto advanced = px::sdk::ParseStudioUi(AdvancedScene());
    Check(advanced.Valid(),
          "named Behavior Graph and Animation contracts must parse");
    Check(advanced.document.behaviorGraph.nodes.size() == 2,
          "Behavior Graph nodes must round-trip");
    Check(advanced.document.behaviorTriggers.size() == 1,
          "Behavior triggers must round-trip");
    Check(advanced.document.animations.has_value() &&
              advanced.document.animations->clips.size() == 1 &&
              advanced.document.animations->clips[0].tracks.size() == 1,
          "Animation clips and tracks must round-trip");

    const std::pair<std::string_view, std::string_view> inspectorShapes[] = {
        {"signalEntry", R"({})"},
        {"action", R"({"action":"game.start","wait":false})"},
        {"sequence", R"({})"},
        {"branch", R"({"condition":true})"},
        {"delay", R"({"seconds":1.25})"},
        {"constant", R"({"value":{"type":"color","value":"#12345678"}})"},
        {"compare", R"({"left":{"type":"vec2","x":1,"y":2},"right":{"type":"nodeReference","nodeId":"33333333-3333-4333-8333-333333333333"},"operator":"GreaterEqual"})"},
        {"boolean", R"({"left":true,"right":false,"operator":"Not"})"},
        {"getVariable", R"({"name":"playerScore"})"},
        {"setVariable", R"({"name":"chapterScore","value":{"type":"rect","x":1,"y":2,"width":3,"height":4}})"},
        {"getProperty", R"({"target":{"type":"nodeReference","nodeId":"33333333-3333-4333-8333-333333333333"},"property":"minimumSize"})"},
        {"setProperty", R"({"target":{"type":"nodeReference","nodeId":"33333333-3333-4333-8333-333333333333"},"property":"offsets","value":{"type":"rect","x":8,"y":0,"width":320,"height":0}})"},
        {"playAnimation", R"({"name":"Focused","wait":false})"},
        {"setAnimationParameter", R"({"name":"speed","value":2.5})"},
        {"travelAnimationState", R"({"state":"Focused","duration":0.4})"},
    };
    for (const auto& [kind, properties] : inspectorShapes) {
        const auto inspector =
            px::sdk::ParseStudioUi(BehaviorInspectorScene(kind, properties));
        if (!inspector.Valid()) {
            std::cerr << "Behavior inspector shape failed Runtime parsing: "
                      << kind << '\n';
            return 1;
        }
        Check(inspector.document.behaviorGraph.nodes.size() == 1,
              "Behavior inspector node must reach the Runtime contract");
    }

    const std::pair<std::string_view, std::string_view> propertyShapes[] = {
        {"opacity", "0.5"},
        {"rotation", "90"},
        {"scale", R"({"type":"vec2","x":1,"y":2})"},
        {"pivot", R"({"type":"vec2","x":0.5,"y":0.5})"},
        {"modulate", R"({"type":"color","value":"#FFFFFFFF"})"},
        {"visibility", R"("Collapsed")"},
        {"enabled", "true"},
        {"offsets", R"({"type":"rect","x":8,"y":0,"width":320,"height":0})"},
        {"minimumSize", R"({"type":"vec2","x":120,"y":32})"},
    };
    for (const auto& [property, value] : propertyShapes) {
        const std::string properties =
            R"({"target":{"type":"nodeReference","nodeId":"33333333-3333-4333-8333-333333333333"},"property":")" +
            std::string(property) + "\",\"value\":" + std::string(value) + "}";
        const auto setProperty = px::sdk::ParseStudioUi(
            BehaviorInspectorScene("setProperty", properties));
        if (!setProperty.Valid()) {
            std::cerr << "SetProperty inspector shape failed Runtime parsing: "
                      << property << '\n';
            return 1;
        }
    }

    std::string unknownBehaviorKind = AdvancedScene();
    const auto signalEntry = unknownBehaviorKind.find("\"kind\":\"signalEntry\"");
    Check(signalEntry != std::string::npos,
          "unknown Behavior kind fixture must be editable");
    unknownBehaviorKind.replace(signalEntry, 20,
                                "\"kind\":\"vendor.removedNode\"");
    const auto unknownBehavior = px::sdk::ParseStudioUi(unknownBehaviorKind);
    Check(!unknownBehavior.Valid() &&
              std::ranges::any_of(
                  unknownBehavior.diagnostics,
                  [](const px::sdk::StudioUiContractDiagnostic& diagnostic) {
                      return diagnostic.code == "PXSDKUI1059";
                  }),
          "unknown Behavior kinds must fail Runtime parsing with a dedicated diagnostic");

    std::string missingGraphEndpoint = AdvancedScene();
    const auto endpoint =
        missingGraphEndpoint.find("88888888-8888-4888-8888-888888888888",
                                  missingGraphEndpoint.find("\"toNodeId\""));
    Check(endpoint != std::string::npos,
          "Behavior endpoint fixture must be editable");
    missingGraphEndpoint.replace(
        endpoint, 36, "abababab-abab-4bab-8bab-abababababab");
    Check(!px::sdk::ParseStudioUi(missingGraphEndpoint).Valid(),
          "Behavior links to missing nodes must be rejected");

    std::string unorderedKeys = AdvancedScene();
    const auto secondTime = unorderedKeys.find("\"time\":1");
    Check(secondTime != std::string::npos,
          "Animation key fixture must be editable");
    unorderedKeys.replace(secondTime, 8, "\"time\":-1");
    Check(!px::sdk::ParseStudioUi(unorderedKeys).Valid(),
          "unordered or negative animation keys must be rejected");

    const auto component = px::sdk::ParseStudioUiComponent(ComponentContract());
    Check(component.Valid(),
          "typed reusable component public contract must parse");
    Check(component.document.componentInterface.properties.size() == 1 &&
              component.document.componentInterface.properties[0].id ==
                  "caption" &&
              component.document.componentInterface.signals.size() == 1 &&
              component.document.componentInterface.signals[0].signal ==
                  "activated" &&
              component.document.componentInterface.slots.size() == 1,
          "component properties, signals and named slots must remain typed");
    const auto complexComponent =
        px::sdk::ParseStudioUiComponent(ComplexComponentContract());
    if (!complexComponent.Valid())
        for (const auto& diagnostic : complexComponent.diagnostics)
            std::cerr << diagnostic.code << "[" << diagnostic.nodeIndex
                      << "]: " << diagnostic.message << '\n';
    Check(complexComponent.Valid() &&
              complexComponent.document.componentInterface.properties.size() == 6,
          "component UUID/resource/token/array/object public values must share the SDK contract");
    auto invalidResource = nlohmann::json::parse(ComplexComponentContract());
    invalidResource["nodes"][0]["runtimeProperties"]["resource"] =
        "Content/UI/card.png";
    invalidResource["componentInterface"]["properties"][2]["defaultValue"] =
        "Content/UI/card.png";
    Check(!px::sdk::ParseStudioUiComponent(invalidResource.dump()).Valid(),
          "component ResourceRef values must reject filesystem paths");
    auto emptyToken = nlohmann::json::parse(ComplexComponentContract());
    emptyToken["nodes"][0]["runtimeProperties"]["style"] = "";
    emptyToken["componentInterface"]["properties"][3]["defaultValue"] = "";
    Check(!px::sdk::ParseStudioUiComponent(emptyToken.dump()).Valid(),
          "component TokenRef values must reject empty identities");
    std::string wrongDefault = ComponentContract();
    const auto defaultValue = wrongDefault.find("\"defaultValue\":\"開始\"");
    Check(defaultValue != std::string::npos,
          "component default fixture must be editable");
    wrongDefault.replace(defaultValue, 27, "\"defaultValue\":42");
    Check(!px::sdk::ParseStudioUiComponent(wrongDefault).Valid(),
          "component default values must match their declared public type");
    std::string sceneOnlyComponent = ComponentContract();
    const auto sceneOnlyEnd = sceneOnlyComponent.rfind("\n    }");
    sceneOnlyComponent.insert(sceneOnlyEnd,
                              R"(,"behaviorTriggers":[])");
    Check(!px::sdk::ParseStudioUiComponent(sceneOnlyComponent).Valid(),
          "component sources must reject scene-only Runtime behavior fields");
    return 0;
}
