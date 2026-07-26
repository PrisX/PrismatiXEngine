#include "Engine/SDK/StudioUi.h"

#include <cstdlib>
#include <iostream>
#include <string>

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

    std::string future = ValidScene();
    future.replace(future.find("\"schemaRevision\":1"), 18,
                   "\"schemaRevision\":2");
    Check(!px::sdk::ParseStudioUi(future).Valid(),
          "future Studio UI schema revision must be rejected");

    std::string unknownAction = ValidScene();
    unknownAction.replace(unknownAction.find("game.start"), 10, "fake.start");
    Check(!px::sdk::ParseStudioUi(unknownAction).Valid(),
          "unknown typed actions must be rejected");

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
    return 0;
}
