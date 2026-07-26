#include "Engine/SDK/StudioUi.h"
#include "Engine/UI/StudioUiAdapter.h"
#include "Engine/UI/UIContext.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Engine/UI/Widgets.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void Check(const bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
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
         "layout":{"mode":"free","x":100,"y":200,"width":240,"height":60,"anchorX":0,"anchorY":0,"pivotX":0.5,"pivotY":0.5,"margin":0,"alignment":"center","sizeRule":"fixed"},
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
           "arguments":{}}
        ],
        "links":[
          {"id":"99999999-9999-4999-8999-999999999999",
           "fromNodeId":"77777777-7777-4777-8777-777777777777","fromPin":"out",
           "toNodeId":"88888888-8888-4888-8888-888888888888","toPin":"in"}
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
    const auto parsed = px::sdk::ParseStudioUi(scene);
    Check(parsed.Valid(), "valid Studio UI must parse before Runtime adaptation");
    std::size_t actions = 0;
    auto runtime = px::ui::BuildStudioUiRuntimeTree(
        parsed.document,
        [](const std::string_view id) -> std::optional<std::string> {
            if (id == "66666666-6666-4666-8666-666666666666")
                return "Content/logo.png";
            return std::nullopt;
        },
        [&actions](const px::sdk::StudioUiAction& action) {
            Check(action.id == "game.start",
                  "Runtime button must dispatch the authored typed action");
            ++actions;
        });
    Check(runtime.Valid(), "Studio UI must produce a Runtime tree");
    Check(runtime.nodeCount == 3, "Runtime tree must retain every authored node");
    Check(runtime.actionBindingCount == 1,
          "Runtime tree must bind the authored button action");
    Check(runtime.behaviorNodeCount == 2 &&
              runtime.behaviorTriggerCount == 1,
          "Runtime adapter must retain Behavior Graph nodes and triggers");
    Check(runtime.animationClipCount == 1 &&
              runtime.animationTrackCount == 1,
          "Runtime adapter must retain Animation clips and tracks");
    Check(runtime.unresolvedAssetIds.empty(), "known asset must resolve");
    const auto buttonId = px::Uuid::Parse("33333333-3333-4333-8333-333333333333");
    const auto imageId = px::Uuid::Parse("55555555-5555-4555-8555-555555555555");
    Check(buttonId.has_value() && imageId.has_value(), "fixture UUIDs must parse");
    auto* button = dynamic_cast<px::ui::Button*>(runtime.root->Find(*buttonId));
    const auto* image = dynamic_cast<const px::ui::TextureRect*>(runtime.root->Find(*imageId));
    Check(button != nullptr && image != nullptr,
          "Runtime tree must use real Button and TextureRect controls");
    Check(button->Offsets() == px::Rect{100, 200, 240, 60},
          "free-layout offsets must match the Studio artboard");
    Check(image->Path() == "Content/logo.png", "image path must use the resolver");
    Check(image->Opacity() == 0.75f, "image opacity must round-trip");
    button->Activate();
    Check(actions == 1, "button activation must reach the Preview action sink");

    Check(px::ui::RegisterBuiltinUITypes().IsOk(),
          "Runtime UI property registry must be available");
    px::ui::UIContext context;
    Check(context.SetRoot(std::move(runtime.root)).IsOk(),
          "Runtime UIContext must install the Studio tree");
    Check(runtime.animations.has_value() &&
              context.SetAnimations(std::move(*runtime.animations), false)
                  .IsOk(),
          "Runtime UIContext must install the Studio animation library");
    Check(context
              .ConfigureTriggers(std::move(runtime.behaviorTriggers),
                                 std::move(runtime.behaviorGraph), "fixture.pxui")
              .IsOk(),
          "Runtime UIContext must install Studio Behavior triggers");
    button = dynamic_cast<px::ui::Button*>(context.Root()->Find(*buttonId));
    Check(button != nullptr, "installed Runtime button must remain typed");
    button->Activate();
    Check(button->Opacity() == 0.25f,
          "Behavior trigger must execute through the real Runtime graph");
    const auto clipId =
        px::Uuid::Parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    Check(clipId.has_value() &&
              context.PreviewAnimation(*clipId, 1.0f, false).IsOk(),
          "Runtime animation controller must preview the authored clip");
    Check(button->Opacity() == 0.4f,
          "Animation sampling must update the real Runtime property");
    return 0;
}
