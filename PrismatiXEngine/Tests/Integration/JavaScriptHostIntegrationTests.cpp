#include "Engine/Animation/Timeline.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/IO/Crypto.h"
#include "Engine/IO/OverlaySignature.h"
#include "Engine/IO/VFS.h"
#include "Engine/Native/NativePluginHost.h"
#include "Engine/Platform/Input.h"
#include "Engine/Progression/GlobalProfile.h"
#include "Engine/Script/JavaScriptHost.h"
#include "Engine/Script/ScriptRawGraphics.h"
#include "Engine/UI/Actions/ActionCatalog.h"
#include "Engine/UI/Actions/ActionDispatcher.h"
#include "Engine/UI/Control.h"
#include "Engine/UI/UIRouter.h"
#include "Engine/VN/Commands/CommandRegistry.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Engine/VN/Runtime/Dialogue.h"
#include "Engine/VN/Runtime/VariableStore.h"
#include "Tests/TestSupport/TestHarness.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace {

void Write(const std::filesystem::path& path, const std::string_view text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) throw std::runtime_error("could not write JavaScript fixture");
}

void TestSandboxAndImmediateParity(px::test::Suite& suite) {
    px::test::TempDirectory temp("javascript-host");
    const auto extensions = temp.path / "Content" / "Extensions";
    std::filesystem::create_directories(extensions);
    Write(extensions / "demo.js", R"js(
        let providerState = { score: 1, migrated: false };
        Engine.RegisterStateProvider("demo-state", 2, {
            capture: () => ({ ...providerState }),
            restore: (state) => {
                if (state.throwRestore) throw new Error("state restore rejected");
                providerState = { ...state };
            },
            migrate: (state, fromVersion, toVersion) => ({
                score: state?.score ?? 0,
                migrated: fromVersion < toVersion
            })
        });
        Engine.RegisterCommand("js.demo.state", (args) => {
            providerState.score = args.score;
        });
        Engine.RegisterCommand("js.demo.echo", (args) => {
            Engine.log(`${args.message}:${args.amount}`);
        });
        Engine.RegisterCommand("js.demo.await", async (args) => {
            Engine.SetVariable("replay.before",
                (Engine.GetVariable("replay.before") ?? 0) + 1, "session");
            Engine.log("async-command:start");
            await Engine.WaitSeconds(args.seconds);
            Engine.SetVariable("replay.middle",
                (Engine.GetVariable("replay.middle") ?? 0) + 1, "session");
            Engine.log("async-command:middle");
            await Engine.WaitSeconds(0.02);
            Engine.log("async-command:done");
        });
        Engine.RegisterCommand("js.demo.awaitAnimation", async (args) => {
            await Engine.WaitAnimation(args.handle);
            Engine.log("async-animation:done");
        });
        Engine.RegisterCommand("js.demo.say", async (args) => {
            await Engine.Say(args.text, args.speaker);
            Engine.log("script-say:done");
        });
        Engine.RegisterCommand("js.demo.debug", async (args) => {
            const state = { score: args.score, nested: { value: "watch-ready" } };
            await Engine.DebugPoint("Content/Extensions/demo.ts", 10,
                                    { state, score: state.score }, "debugCommand");
            state.score += 1;
            await Engine.DebugPoint("Content/Extensions/demo.ts", 11,
                                    { state, score: state.score }, "debugCommand");
            Engine.log(`debug-done:${state.score}`);
        });
        Engine.RegisterAction("js.demo.action", (args, context) => {
            Engine.log(`${args.message}:${context.preview}`);
        });
        Engine.RegisterAction("js.demo.asyncAction", async (args, context) => {
            Engine.log(`async-action:start:${context.scene}`);
            await Engine.WaitSeconds(args.seconds);
            Engine.log("async-action:middle");
            await Engine.WaitSeconds(0.01);
            Engine.log("async-action:done");
        });
        Engine.RegisterAction("js.demo.failingAction", async () => {
            await Engine.WaitSeconds(0);
            throw new Error("intentional async failure");
        });
        Engine.On("js.demo.event", async (payload) => {
            await Promise.resolve();
            Engine.log(`event:${payload.value}:${payload.nested.count}:${payload.flags[1]}`);
        });
        Engine.On("js.demo.event", () => Engine.log("event:second"));
        Engine.On("js.demo.recursive", () => Engine.Emit("js.demo.recursive"));
    )js");
    Write(extensions / "default.pxextension", R"json({
        "format": "PrismatiXExtension",
        "schemaRevision": 1,
        "language": "javascript",
        "id": "js-demo",
        "version": "1.0.0",
        "requiredEngineVersion": ">=0.1.0 <0.2.0",
        "entry": "demo.js",
        "capabilities": ["runtime", "ui", "persistence"],
        "safety": {
            "previewSafe": true,
            "deterministic": true,
            "seekSafe": false,
            "rollbackSafe": false
        },
        "commands": [
            {
                "id": "js.demo.state",
                "displayName": "State",
                "description": "Extension state provider fixture",
                "category": "Tests",
                "await": false,
                "rollback": "reversible",
                "parameters": [
                    {"name":"score","type":"integer","required":true}
                ]
            },
            {
                "id": "js.demo.echo",
                "displayName": "Echo",
                "description": "Immediate JavaScript command",
                "category": "Tests",
                "await": false,
                "rollback": "reversible",
                "parameters": [
                    {"name":"message","type":"string","required":true},
                    {"name":"amount","type":"integer","default":1,
                     "range":{"minimum":0,"maximum":10}}
                ]
            },
            {
                "id": "js.demo.await",
                "displayName": "Await",
                "description": "Checkpointed JavaScript command",
                "category": "Tests",
                "await": true,
                "rollback": "boundary",
                "parameters": [
                    {"name":"seconds","type":"number","required":true,
                     "range":{"minimum":0,"maximum":1}}
                ]
            },
            {
                "id": "js.demo.awaitAnimation",
                "displayName": "Await Animation",
                "description": "Runtime-owned animation wait",
                "category": "Tests",
                "await": true,
                "rollback": "boundary",
                "parameters": [
                    {"name":"handle","type":"integer","required":true}
                ]
            },
            {
                "id": "js.demo.say",
                "displayName": "Say",
                "description": "Checkpointed dialogue",
                "category": "Tests",
                "await": true,
                "rollback": "boundary",
                "parameters": [
                    {"name":"text","type":"string","required":true},
                    {"name":"speaker","type":"string","required":false}
                ]
            },
            {
                "id": "js.demo.debug",
                "displayName": "Debug",
                "description": "Cooperative debugger probe fixture",
                "category": "Tests",
                "await": true,
                "rollback": "boundary",
                "parameters": [
                    {"name":"score","type":"integer","required":true}
                ]
            }
        ],
        "actions": [
            {
                "id": "js.demo.action",
                "displayName": "JavaScript Action",
                "category": "Tests",
                "reentry": "restart",
                "parameters": [{"name":"message","type":"string","required":true}]
            },
            {
                "id": "js.demo.asyncAction",
                "displayName": "Asynchronous JavaScript Action",
                "category": "Tests",
                "reentry": "allow",
                "parameters": [{"name":"seconds","type":"number","required":true}]
            },
            {
                "id": "js.demo.failingAction",
                "displayName": "Failing JavaScript Action",
                "category": "Tests",
                "reentry": "allow",
                "parameters": []
            }
        ]
    })json");
    Write(extensions / "denied.pxextension", R"json({
        "format":"PrismatiXExtension",
        "schemaRevision":1,
        "language":"javascript",
        "id":"denied",
        "version":"1.0.0",
        "requiredEngineVersion":"^0.1.0",
        "entry":"demo.js",
        "capabilities":["filesystem"],
        "commands":[],
        "actions":[]
    })json");
    Write(extensions / "incompatible.js",
          "Engine.SetVariable('version.executed', true, 'session');\n");
    Write(extensions / "incompatible.pxextension", R"json({
        "format":"PrismatiXExtension","schemaRevision":1,
        "language":"javascript","id":"incompatible","version":"1.0.0",
        "requiredEngineVersion":">=0.3.0","entry":"incompatible.js",
        "capabilities":["runtime"],"commands":[],"actions":[]
    })json");
    Write(extensions / "capability.js", R"js(
        Engine.RegisterAction("capability.denied", () => {
            Engine.SetVariable("capability.executed", true, "session");
        });
    )js");
    Write(extensions / "capability.pxextension", R"json({
        "format":"PrismatiXExtension","schemaRevision":1,
        "language":"javascript","id":"capability","version":"1.0.0",
        "requiredEngineVersion":"^0.1.0","entry":"capability.js",
        "capabilities":["ui"],
        "safety":{"previewSafe":true,"deterministic":true,"seekSafe":true,"rollbackSafe":true},
        "commands":[],"actions":[{"id":"capability.denied","parameters":[]}]
    })json");
    Write(extensions / "ghost.js", R"js(
        Engine.RegisterCommand("ghost.command", () => {});
        throw new Error("load must roll back");
    )js");
    Write(extensions / "ghost.pxextension", R"json({
        "format":"PrismatiXExtension","schemaRevision":1,
        "language":"javascript","id":"ghost","version":"1.0.0",
        "requiredEngineVersion":"^0.1.0","entry":"ghost.js",
        "capabilities":["runtime"],
        "safety":{"previewSafe":true,"deterministic":true,"seekSafe":true,"rollbackSafe":true},
        "commands":[{"id":"ghost.command","parameters":[]}],"actions":[]
    })json");
    Write(extensions / "realm-a.js", R"js(
        globalThis.privateValue = "realm-a";
        Engine.RegisterCommand("realm.a", () => Engine.log(privateValue));
    )js");
    Write(extensions / "realm-b.js", R"js(
        globalThis.privateValue = "realm-b";
        Engine.RegisterCommand("realm.b", () => Engine.log(privateValue));
    )js");
    const auto realmManifest = [](const std::string_view id,
                                  const std::string_view entry,
                                  const std::string_view command) {
        return std::string("{\"format\":\"PrismatiXExtension\",\"schemaRevision\":1,") +
            "\"language\":\"javascript\",\"id\":\"" + std::string(id) +
            "\",\"version\":\"1.0.0\",\"requiredEngineVersion\":\"^0.1.0\"," +
            "\"entry\":\"" + std::string(entry) +
            "\",\"capabilities\":[\"runtime\"]," +
            "\"safety\":{\"previewSafe\":true,\"deterministic\":true," +
            "\"seekSafe\":true,\"rollbackSafe\":true},\"commands\":[{" +
            "\"id\":\"" + std::string(command) +
            "\",\"parameters\":[]}],\"actions\":[]}";
    };
    Write(extensions / "realm-a.pxextension",
          realmManifest("realm-a", "realm-a.js", "realm.a"));
    Write(extensions / "realm-b.pxextension",
          realmManifest("realm-b", "realm-b.js", "realm.b"));
    Write(extensions / "module-helper.js",
          "export const moduleMessage = 'module-isolated';\n");
    Write(extensions / "module-entry.js", R"js(
        import { moduleMessage } from "./module-helper.js";
        Engine.RegisterCommand("module.command", () => Engine.log(moduleMessage));
    )js");
    Write(extensions / "module.pxextension", R"json({
        "format":"PrismatiXExtension","schemaRevision":1,
        "language":"javascript","id":"module","version":"1.0.0",
        "requiredEngineVersion":"^0.1.0","entry":"module-entry.js",
        "modules":["module-helper.js"],"capabilities":["runtime"],
        "safety":{"previewSafe":true,"deterministic":true,"seekSafe":true,"rollbackSafe":true},
        "commands":[{"id":"module.command","parameters":[]}],"actions":[]
    })json");
    Write(extensions / "undeclared.js", "export const value = true;\n");
    Write(extensions / "bad-module-entry.js", R"js(
        import { value } from "./undeclared.js";
        Engine.RegisterCommand("bad-module.command", () => value);
    )js");
    Write(extensions / "bad-module.pxextension", R"json({
        "format":"PrismatiXExtension","schemaRevision":1,
        "language":"javascript","id":"bad-module","version":"1.0.0",
        "requiredEngineVersion":"^0.1.0","entry":"bad-module-entry.js",
        "capabilities":["runtime"],
        "safety":{"previewSafe":true,"deterministic":true,"seekSafe":true,"rollbackSafe":true},
        "commands":[{"id":"bad-module.command","parameters":[]}],"actions":[]
    })json");
    Write(temp.path / "Content" / "foreign.js",
          "export const escaped = true;\n");
    Write(extensions / "traversal-entry.js", R"js(
        import { escaped } from "../foreign.js";
        Engine.RegisterCommand("traversal.command", () => escaped);
    )js");
    Write(extensions / "traversal.pxextension", R"json({
        "format":"PrismatiXExtension","schemaRevision":1,
        "language":"javascript","id":"traversal","version":"1.0.0",
        "requiredEngineVersion":"^0.1.0","entry":"traversal-entry.js",
        "capabilities":["runtime"],
        "safety":{"previewSafe":true,"deterministic":true,"seekSafe":true,"rollbackSafe":true},
        "commands":[{"id":"traversal.command","parameters":[]}],"actions":[]
    })json");

    px::io::VFS vfs;
    vfs.MountDirectory(temp.path.string());
    suite.Require(vfs.Exists("Content/Extensions/default.pxextension"),
                  "fixture directory should mount through the production VFS");
    std::vector<px::script::ConsoleMessage> console;
    px::vn::VariableStore variables;
    px::progress::GlobalProfile profile;
    px::ui::UIRouter routes;
    px::animation::TimelinePlayer timeline;
    px::graphics::AssetCache assets(nullptr, vfs);
    px::graphics::Renderer2D renderer(nullptr, assets);
    px::vn::Stage stage(renderer, assets);
    px::ui::RouteTransition drivenTransition;
    px::ui::RouteTransitionHandle drivenHandle = 0;
    const auto routeFactory = []() -> px::Result<std::unique_ptr<px::ui::Control>> {
        return px::Result<std::unique_ptr<px::ui::Control>>::Success(
            std::make_unique<px::ui::Control>());
    };
    suite.Require(static_cast<bool>(routes.Register("title", routeFactory)),
                  "runtime route fixture should register");
    routes.SetTransitionDriver({
        .play = [&](const px::ui::RouteTransition& transition) {
            drivenTransition = transition;
            drivenHandle = 73;
            return drivenHandle;
        },
        .playing = [&](const px::ui::RouteTransitionHandle handle) {
            return handle == drivenHandle;
        },
        .stop = [&](const px::ui::RouteTransitionHandle handle) {
            return handle == drivenHandle;
        },
        .cancel = [&](const px::ui::RouteTransitionHandle handle) {
            return handle == drivenHandle;
        }});
    px::script::ScriptServices services;
    services.vfs = &vfs;
    services.variables = &variables;
    services.profile = &profile;
    services.routes = &routes;
    services.timeline = &timeline;
    services.renderer = &renderer;
    services.stage = &stage;
    int advanceRequests = 0;
    int selectedChoice = -1;
    std::uint64_t replayedSequence = 0;
    std::uint64_t rollbackSequence = 0;
    std::string updatedSetting;
    px::Variant updatedSettingValue;
    services.queryDialogue = [] {
        return px::Variant(px::VariantObject{
            {"active", px::Variant(true)},
            {"speaker", px::Variant("Yuki")},
            {"fullText", px::Variant("Hello")},
            {"displayText", px::Variant("Hel")},
            {"voice", px::Variant("Content/Voice/001.ogg")},
            {"currentChar", px::Variant(3)},
            {"totalChars", px::Variant(5)},
            {"progress", px::Variant(0.6)},
            {"finished", px::Variant(false)},
            {"textColor", px::Variant(px::VariantObject{
                 {"r", px::Variant(245)}, {"g", px::Variant(248)},
                 {"b", px::Variant(255)}, {"a", px::Variant(255)}})},
            {"outlineColor", px::Variant(px::VariantObject{
                 {"r", px::Variant(0)}, {"g", px::Variant(0)},
                 {"b", px::Variant(0)}, {"a", px::Variant(255)}})},
            {"effect", px::Variant("wave")},
            {"effectProgress", px::Variant(0.4)}});
    };
    services.queryChoices = [] {
        return px::Variant(px::VariantArray{px::Variant(px::VariantObject{
            {"index", px::Variant(0)}, {"text", px::Variant("Go")},
            {"target", px::Variant("route-a")},
            {"sourceId", px::Variant("story")},
            {"operationId", px::Variant("choice-1")}})});
    };
    services.queryBacklog = [] {
        return px::Variant(px::VariantArray{px::Variant(px::VariantObject{
            {"sequence", px::Variant(17)}, {"speaker", px::Variant("Yuki")},
            {"text", px::Variant("Hello")},
            {"voice", px::Variant("Content/Voice/001.ogg")},
            {"isChoice", px::Variant(false)},
            {"sourceId", px::Variant("story")},
            {"operationId", px::Variant("line-1")}})});
    };
    services.querySettings = [] {
        return px::Variant(px::VariantObject{
            {"bgmVolume", px::Variant(96)}, {"seVolume", px::Variant(110)},
            {"voiceVolume", px::Variant(128)}, {"textSpeedMs", px::Variant(28)},
            {"autoWaitMs", px::Variant(1400)}, {"skipReadOnly", px::Variant(true)},
            {"fullscreen", px::Variant(false)}, {"windowWidth", px::Variant(1280)},
            {"windowHeight", px::Variant(720)}, {"language", px::Variant("zh-TW")},
            {"textScale", px::Variant(1.0)}, {"highContrast", px::Variant(false)},
            {"reducedMotion", px::Variant(false)}, {"selfVoicing", px::Variant(false)},
            {"supportedLocales", px::Variant(px::VariantArray{
                 px::Variant("zh-TW"), px::Variant("ja-JP")})}});
    };
    services.advanceDialogue = [&] { ++advanceRequests; return true; };
    px::vn::Dialogue authoredDialogue;
    int authoredPresentations = 0;
    px::vn::Command authoredAnchor;
    services.presentScriptSay = [&](const px::vn::Command& say) {
        ++authoredPresentations;
        authoredAnchor = say;
        authoredDialogue.SetText(say.Get("speaker"), say.Get("text"), 30,
                                 {245, 248, 255, 255}, {0, 0, 0, 255});
        return px::Status::Ok();
    };
    services.selectChoice = [&](const int index) {
        selectedChoice = index;
        return index == 0;
    };
    services.replayBacklogVoice = [&](const std::uint64_t sequence) {
        replayedSequence = sequence;
        return sequence == 17;
    };
    services.rollbackBacklog = [&](const std::uint64_t sequence) {
        rollbackSequence = sequence;
        return sequence == 17;
    };
    services.updateSetting = [&](const std::string_view name,
                                 const px::Variant& value) {
        updatedSetting = name;
        updatedSettingValue = value.Clone();
        return true;
    };
    services.console = [&console](const px::script::ConsoleMessage& message) {
        console.push_back(message);
    };
    px::script::JavaScriptHost host(services);
    suite.Equal("javascript", host.BackendId(),
                "embedded host must identify the JavaScript backend");
    stage.SetBackground("Assets/background.webp", false);
    stage.SetCharacter("hero", "Assets/hero.webp", 2, false);
    stage.SetCharacter("friend", "Assets/friend.webp", 3, false);
    suite.Require(host.RunString("Engine.ClearCharacters(false);"),
                  "script can clear all character actors without naming each one");
    suite.Expect(stage.CaptureState().actors.empty() &&
                     stage.BackgroundPath() == "Assets/background.webp",
                 "script clear-all preserves the background");
    suite.Expect(host.RunString(R"js(
        if (typeof require !== "undefined" || typeof Date !== "undefined" ||
            typeof eval !== "undefined" || typeof Function !== "undefined") {
            throw new Error("sandbox exposed a host or nondeterministic primitive");
        }
        let blocked = false;
        try { Math.random(); } catch (_) { blocked = true; }
        if (!blocked) throw new Error("Math.random was not blocked");
        console.log("sandbox-ready", 7);
        console.warn("sandbox-warning");
    )js", "javascript-sandbox"),
                 "JavaScript sandbox should execute deterministic source");
    suite.Expect(console.size() == 2 &&
                     console[0].level == px::script::ConsoleLevel::Info &&
                     console[0].text == "sandbox-ready 7" &&
                     console[1].level == px::script::ConsoleLevel::Warning,
                 "console bridge should preserve messages and levels");
    suite.Expect(host.RunString(R"js(
        const dialogue = Engine.GetDialogue();
        const choices = Engine.GetChoices();
        const backlog = Engine.GetBacklog();
        const settings = Engine.GetSettings();
        if (dialogue.speaker !== "Yuki" || dialogue.displayText !== "Hel" ||
            choices[0].operationId !== "choice-1" ||
            backlog[0].sequence !== 17 || settings.language !== "zh-TW") {
            throw new Error("VN model snapshots were not exposed");
        }
        if (!Engine.AdvanceDialogue() || !Engine.SelectChoice(0) ||
            !Engine.ReplayBacklogVoice(17) || !Engine.RollbackBacklog(17) ||
            !Engine.SetSetting("textScale", 1.25)) {
            throw new Error("VN model operations were not exposed");
        }
    )js", "javascript-vn-models") &&
                     advanceRequests == 1 && selectedChoice == 0 &&
                     replayedSequence == 17 && rollbackSequence == 17 &&
                     updatedSetting == "textScale" &&
                     updatedSettingValue == px::Variant(1.25),
                 "public VN models and item operations should bridge through ScriptServices");
    suite.Expect(host.RunString(R"js(
        Engine.SetVariable("js.runtime", { score: 7, flags: [true, "ready"] }, "session");
        const restored = Engine.GetVariable("js.runtime");
        if (restored.score !== 7 || restored.flags[1] !== "ready") {
            throw new Error("typed variable round-trip failed");
        }
        if (!Engine.ResourceExists("Content/Extensions/default.pxextension") ||
            !Engine.ReadResourceText("Content/Extensions/default.pxextension").includes("PrismatiXExtension")) {
            throw new Error("VFS bridge failed");
        }
        Engine.MarkSeen("js.scene");
        Engine.UnlockCG("js.cg");
        if (!Engine.HasSeen("js.scene") || !Engine.CGUnlocked("js.cg")) {
            throw new Error("profile bridge failed");
        }
        if (!Engine.PushRoute("title")) throw new Error("route bridge failed");
        if (!Engine.SetRouteTransition("title", "title", {
              preset: "slide-left", durationSeconds: 0.35
            }) || !Engine.ReplaceRoute("title")) {
            throw new Error("route transition bridge failed");
        }
        Engine.SetBackground("Content/Images/outgoing.png", false);
        Engine.SetBackgroundRule("Content/Images/incoming.png",
                                 "Content/Rules/dissolve.png", 750, 48);
        Engine.SetLayerTransform("foreground", "Content/Images/card.png",
                                 14, 28, 1.25, 0.75, 20, 210, 4);
        if (!Engine.SetStageGroup("scene-root") ||
            !Engine.SetStageNodeParent("foreground", "scene-root") ||
            !Engine.SetStageNodeTransform("scene-root", {
              x: 3, y: -4, scaleX: 0.9, scaleY: 1.1,
              rotation: 5, opacity: 0.8
            }) || !Engine.SetStageNodeOrder("foreground", 4, 9) ||
            !Engine.SetStageNodeVisibility("foreground", true) ||
            !Engine.SetParticleEmitter("snow", "snow", {
              seed: 17, rate: 50, maxParticles: 128, z: -1,
              opacity: 0.75, wind: 12, speed: 0.9, size: 1.2,
              texture: "Content/Images/card.png",
              atlas: {columns: 2, rows: 2, firstFrame: 0, frameCount: 4},
              spawnShape: "ellipse",
              position: {x: [0.1, 0.9], y: [0, 0.2]},
              velocity: {x: [-20, 20], y: [30, 60]},
              acceleration: {x: [-2, 2], y: [4, 8]},
              lifetime: [3, 6], rotation: [-30, 30],
              angularVelocity: [-90, 90], scale: [0.5, 1.5],
              initialOpacity: [0.4, 1], gravity: 9, variation: 0.2,
              burst: 16, loop: false, duration: 2,
              scaleOverLifetime: [{time: 0, value: 0.5}, {time: 1, value: 1}],
              opacityOverLifetime: [{time: 0, value: 1}, {time: 1, value: 0}],
              colorOverLifetime: [
                {time: 0, value: [255, 255, 255, 255]},
                {time: 1, value: [180, 210, 255, 0]}]
            })) {
            throw new Error("stage graph/particle bridge failed");
        }
        Engine.SetCamera(12, -8, 1.2);
        Engine.SetScreenEffect("fade", 0.4);
        if (!Engine.RegisterScreenEffect("js-tile-transition", {
              operator: "tiles", columns: 9, rows: 6,
              stagger: 0.3, order: "column-major"
            })) {
            throw new Error("screen effect registration bridge failed");
        }
        Engine.log("runtime-context-ready");
    )js", "javascript-runtime-context"),
                 "JavaScript RuntimeContext should bridge typed engine services");
    const auto* runtimeValue = variables.GetValue("js.runtime");
    const auto stageState = stage.CaptureState();
    const auto foreground = std::ranges::find_if(
        stageState.layers, [](const px::vn::Stage::SavedLayer& layer) {
            return layer.name == "foreground";
        });
    suite.Expect(runtimeValue && runtimeValue->AsObject() &&
                     profile.HasSeen("js.scene") && profile.CGUnlocked("js.cg") &&
                     routes.CurrentRoute() == "title" &&
                     routes.ActiveTransition() && drivenHandle == 73 &&
                     drivenTransition.preset == "slide-left" &&
                     std::abs(drivenTransition.durationSeconds - 0.35f) < 0.001f &&
                     stageState.background == "Content/Images/incoming.png" &&
                     foreground != stageState.layers.end() &&
                     std::abs(foreground->scale - 1.25f) < 0.001f &&
                     std::abs(foreground->scaleY - 0.75f) < 0.001f &&
                     std::abs(foreground->rotation - 20.0f) < 0.001f &&
                     stageState.nodes.size() == 2 &&
                     stageState.particleEmitters.size() == 1 &&
                     stageState.particleEmitters.front().spec.seed == 17 &&
                     stageState.particleEmitters.front().spec.advanced &&
                     stageState.particleEmitters.front().spec.texture ==
                         "Content/Images/card.png" &&
                     stageState.particleEmitters.front().spec.burst == 16 &&
                     std::abs(stageState.cameraX - 12.0f) < 0.001f &&
                     std::abs(stageState.cameraY + 8.0f) < 0.001f &&
                     std::abs(stageState.cameraZoom - 1.2f) < 0.001f &&
                     stageState.screenEffects.contains("fade") &&
                     renderer.HasScreenEffect("js-tile-transition") &&
                     console.back().text == "runtime-context-ready",
                 "RuntimeContext stage, transform, camera, effect, and route bindings should reach C++ services");
    suite.Expect(!host.LoadExtensionManifest(
                     "Content/Extensions/denied.pxextension"),
                 "undeclared host capabilities must fail closed");
    suite.Expect(!host.LoadExtensionManifest(
                     "Content/Extensions/incompatible.pxextension") &&
                     !variables.GetValue("version.executed"),
                 "requiredEngineVersion must reject before evaluating extension code");
    suite.Require(host.LoadExtensionManifest(
                      "Content/Extensions/capability.pxextension"),
                  "a UI-only extension should load its declared Action");
    px::ui::ActionInvocation deniedInvocation;
    deniedInvocation.action = "capability.denied";
    suite.Expect(!host.InvokeAction(deniedInvocation) &&
                     !variables.GetValue("capability.executed"),
                 "undeclared runtime capability must reject the actual Engine API call");
    suite.Expect(!host.LoadExtensionManifest(
                     "Content/Extensions/ghost.pxextension") &&
                     !px::vn::CommandRegistry::Global().Find("ghost.command"),
                 "failed extension evaluation must not leave a ghost command");
    suite.Require(host.LoadExtensionManifest(
                      "Content/Extensions/realm-a.pxextension") &&
                      host.LoadExtensionManifest(
                          "Content/Extensions/realm-b.pxextension"),
                  "independent extension realms should load");
    px::vn::Command realmA;
    realmA.type = "realm.a";
    px::vn::Command realmB;
    realmB.type = "realm.b";
    suite.Require(host.InvokeCommand(realmA) && host.InvokeCommand(realmB) &&
                      console[console.size() - 2].text == "realm-a" &&
                      console.back().text == "realm-b",
                  "extension globals must be isolated by JSRuntime");
    suite.Require(host.LoadExtensionManifest(
                      "Content/Extensions/module.pxextension"),
                  "declared relative ES modules should load in the extension realm");
    px::vn::Command moduleCommand;
    moduleCommand.type = "module.command";
    suite.Expect(host.InvokeCommand(moduleCommand) &&
                     console.back().text == "module-isolated",
                 "an extension should execute exports from its declared module graph");
    suite.Expect(!host.LoadExtensionManifest(
                     "Content/Extensions/bad-module.pxextension") &&
                     !px::vn::CommandRegistry::Global().Find("bad-module.command"),
                 "undeclared imports must fail before registration without ghost commands");
    suite.Expect(!host.LoadExtensionManifest(
                     "Content/Extensions/traversal.pxextension") &&
                     !px::vn::CommandRegistry::Global().Find("traversal.command"),
                 "extension imports must not escape their package root or leave ghost commands");
    Write(extensions / "realm-a.js", R"js(
        globalThis.privateValue = "realm-a-reloaded";
        Engine.RegisterCommand("realm.a", () => Engine.log(privateValue));
    )js");
    suite.Require(host.LoadExtensionManifest(
                      "Content/Extensions/realm-a.pxextension") &&
                      host.InvokeCommand(realmA) &&
                      console.back().text == "realm-a-reloaded",
                  "hot reload must atomically replace a source and its realm");
    suite.Require(host.LoadExtensionManifest(
                      "Content/Extensions/default.pxextension"),
                  "strict JavaScript extension manifest should load");

    const auto initialExtensionState = host.CaptureExtensionState();
    suite.Require(initialExtensionState &&
                      initialExtensionState.Value().size() == 1 &&
                      initialExtensionState.Value().front().sourceId == "js-demo" &&
                      initialExtensionState.Value().front().providerId ==
                          "demo-state" &&
                      initialExtensionState.Value().front().version == 2,
                  "extension state providers should capture versioned realm-owned state");
    px::vn::Command mutateExtensionState;
    mutateExtensionState.type = "js.demo.state";
    mutateExtensionState.typedArgs["score"] = px::Variant(std::int64_t{7});
    suite.Require(host.InvokeCommand(mutateExtensionState),
                  "extension state fixture should mutate its private closure");
    suite.Require(static_cast<bool>(host.RestoreExtensionState(
                      initialExtensionState.Value())),
                  "captured extension state should restore transactionally");
    auto migratedExtensionState = initialExtensionState.Value();
    migratedExtensionState.front().version = 1;
    migratedExtensionState.front().state = px::Variant(px::VariantObject{
        {"score", px::Variant(std::int64_t{5})}});
    suite.Require(static_cast<bool>(host.RestoreExtensionState(
                      migratedExtensionState)),
                  "older extension state should pass through the provider migration");
    const auto afterMigration = host.CaptureExtensionState();
    const auto* migratedState = afterMigration
                                    ? afterMigration.Value().front().state.AsObject()
                                    : nullptr;
    suite.Require(migratedState && migratedState->at("score") ==
                                       px::Variant(std::int64_t{5}) &&
                      migratedState->at("migrated") == px::Variant(true),
                  "provider migration should deterministically produce the current schema");
    auto rejectedExtensionState = afterMigration.Value();
    rejectedExtensionState.front().state = px::Variant(px::VariantObject{
        {"throwRestore", px::Variant(true)}});
    suite.Expect(!host.RestoreExtensionState(rejectedExtensionState),
                 "failed provider restore should fail the checkpoint transaction");
    const auto afterRejectedRestore = host.CaptureExtensionState();
    suite.Expect(afterRejectedRestore &&
                     afterRejectedRestore.Value().front().state ==
                         afterMigration.Value().front().state,
                 "failed provider restore should roll back all extension realms");

    const auto* commandDescriptor =
        px::vn::CommandRegistry::Global().Find("js.demo.echo");
    suite.Expect(commandDescriptor && commandDescriptor->parameters.size() == 2 &&
                     commandDescriptor->parameters[1].hasDefault &&
                     commandDescriptor->parameters[1].minimum == 0.0 &&
                     commandDescriptor->parameters[1].maximum == 10.0,
                 "manifest should publish typed command metadata");
    const auto* actionDescriptor =
        px::ui::ActionCatalog::Global().Find("js.demo.action");
    suite.Expect(actionDescriptor && actionDescriptor->providerId == "script" &&
                     actionDescriptor->origin ==
                         px::ui::ActionOrigin::ScriptExtension,
                 "manifest should publish provider-neutral Action metadata");

    px::vn::Command command;
    command.type = "js.demo.echo";
    command.typedArgs["message"] = px::Variant(std::string("hello"));
    command.typedArgs["amount"] = px::Variant(std::int64_t{3});
    suite.Expect(host.InvokeCommand(command),
                 "typed JavaScript command should execute through ScriptHost");
    suite.Require(static_cast<bool>(host.Emit(
                      "js.demo.event",
                      px::Variant(px::VariantObject{
                          {"value", px::Variant(std::string("ok"))},
                          {"nested", px::Variant(px::VariantObject{
                              {"count", px::Variant(std::int64_t{2})}})},
                          {"flags", px::Variant(px::VariantArray{
                              px::Variant(true),
                              px::Variant(std::string("typed"))})}}))),
                  "typed JavaScript event should dispatch successfully");

    px::ui::ActionDispatcher actions;
    suite.Require(static_cast<bool>(
                      actions.RegisterProvider(host.CreateActionProvider())),
                  "JavaScript Action provider should register");
    px::ui::ActionInvocation invocation;
    invocation.action = "js.demo.action";
    invocation.arguments["message"] = px::Variant(std::string("action"));
    invocation.context.preview = true;
    suite.Expect(static_cast<bool>(actions.Dispatch(std::move(invocation))),
                 "typed JavaScript Action should execute through ActionDispatcher");
    suite.Expect(console.size() >= 6 &&
                     console[console.size() - 4].text == "hello:3" &&
                     console[console.size() - 3].text == "event:ok:2:typed" &&
                     console[console.size() - 2].text == "event:second" &&
                     console.back().text == "action:true",
                 "command, event, and Action callbacks should receive runtime values");
    suite.Expect(!host.Emit("js.demo.recursive") &&
                     host.RunString("Engine.log('responsive-after-event-budget')",
                                    "event-budget-recovery"),
                 "recursive event emission must be budgeted without freezing the host");

    px::vn::Command asynchronousCommand;
    asynchronousCommand.type = "js.demo.await";
    asynchronousCommand.typedArgs["seconds"] = px::Variant(0.05);
    suite.Require(host.InvokeCommand(asynchronousCommand) &&
                      host.HasPendingCommand(),
                  "async command should suspend on an engine-owned timer");
    host.Update(0.02f);
    const auto commandCheckpoint = host.CapturePending();
    suite.Require(commandCheckpoint.size() == 1 &&
                      commandCheckpoint.front().yieldIndex == 1 &&
                      commandCheckpoint.front().waitKind == "timer" &&
                      std::abs(commandCheckpoint.front().remainingSeconds - 0.03f) <
                          0.001f,
                  "command checkpoint should preserve its exact yield and timer state");
    px::script::PendingActionState incompatibleAction;
    incompatibleAction.id = 99;
    incompatibleAction.invocation.action = "js.missing.action";
    incompatibleAction.yieldIndex = 1;
    incompatibleAction.waitKind = "timer";
    incompatibleAction.remainingSeconds = 0.01f;
    suite.Expect(
        !host.RestoreCheckpoint(commandCheckpoint, {incompatibleAction}) &&
            host.CapturePending().size() == 1 &&
            host.CapturePending().front().command.type ==
                commandCheckpoint.front().command.type &&
            host.CapturePending().front().yieldIndex ==
                commandCheckpoint.front().yieldIndex,
        "combined JavaScript checkpoint failure must preserve the active command continuation");
    host.CancelPending();
    suite.Require(!host.HasPendingCommand() &&
                      static_cast<bool>(host.RestorePending(commandCheckpoint)),
                  "command continuation should replay to its saved yield boundary");
    suite.Require(variables.GetValue("replay.before") &&
                      *variables.GetValue("replay.before")->TryGet<std::int64_t>() == 1,
                  "checkpoint replay must not repeat variable side effects before the await");
    host.Update(0.04f);
    const auto secondCommandCheckpoint = host.CapturePending();
    suite.Require(secondCommandCheckpoint.size() == 1 &&
                      secondCommandCheckpoint.front().yieldIndex == 2 &&
                      secondCommandCheckpoint.front().waitKind == "timer",
                  "resumed command should advance to the next deterministic boundary");
    host.CancelPending();
    suite.Require(static_cast<bool>(host.RestorePending(secondCommandCheckpoint)) &&
                      variables.GetValue("replay.middle") &&
                      *variables.GetValue("replay.middle")->TryGet<std::int64_t>() == 1,
                  "later checkpoint replay must validate its full journal without replaying mutations");
    host.Update(0.03f);
    suite.Expect(!host.HasPendingCommand() && console.back().text == "async-command:done",
                 "async command should complete after all waits expire");

    px::vn::Command sayCommand;
    sayCommand.type = "js.demo.say";
    sayCommand.sourceId = "story-command";
    sayCommand.operationId = "story-operation";
    sayCommand.typedArgs["text"] = px::Variant("Checkpointed line");
    sayCommand.typedArgs["speaker"] = px::Variant("Yuki");
    suite.Require(host.InvokeCommand(sayCommand) && host.HasPendingCommand() &&
                      authoredPresentations == 1 &&
                      authoredAnchor.sourceId == sayCommand.sourceId &&
                      authoredAnchor.operationId == sayCommand.operationId &&
                      authoredDialogue.State().fullText == "Checkpointed line",
                  "script say should use the containing Story command anchor");
    const auto sayCheckpoint = host.CapturePending();
    suite.Require(sayCheckpoint.size() == 1 &&
                      sayCheckpoint.front().waitKind == "dialogue" &&
                      sayCheckpoint.front().remainingSeconds == 0.0f,
                  "script say should persist a dialogue wait");
    host.CancelPending();
    suite.Require(static_cast<bool>(host.RestorePending(sayCheckpoint)) &&
                      authoredPresentations == 1,
                  "dialogue checkpoint replay must not present or play the line twice");
    suite.Require(host.AdvanceDialogueWait(authoredDialogue) &&
                      authoredDialogue.Finished() && host.HasPendingCommand(),
                  "first advance should reveal the rest of the authored line");
    suite.Require(host.AdvanceDialogueWait(authoredDialogue) &&
                      host.CapturePending().front().remainingSeconds == 1.0f,
                  "second advance should mark the dialogue wait ready");
    const auto advancedSayCheckpoint = host.CapturePending();
    host.CancelPending();
    suite.Require(static_cast<bool>(host.RestorePending(advancedSayCheckpoint)) &&
                      authoredPresentations == 1,
                  "restoring after Advance must preserve the ready wait without replaying the line");
    host.Update(0.0f);
    suite.Expect(!host.HasPendingCommand() &&
                     console.back().text == "script-say:done",
                 "authored command should resume after player advance");

    auto invalidCommandCheckpoint = commandCheckpoint;
    invalidCommandCheckpoint.front().waitKind = "animation";
    suite.Expect(!host.RestorePending(invalidCommandCheckpoint) &&
                     !host.HasPendingCommand(),
                 "checkpoint replay must fail closed when the await structure changes");

    px::animation::AnimationClip animation;
    animation.id = px::Uuid::FromName("javascript-await-animation");
    animation.name = "JavaScript await fixture";
    animation.duration = 0.1f;
    suite.Require(static_cast<bool>(timeline.Register(animation)),
                  "animation wait fixture should register");
    const auto animationHandle = timeline.Play(animation.id, true);
    px::vn::Command animationCommand;
    animationCommand.type = "js.demo.awaitAnimation";
    animationCommand.typedArgs["handle"] =
        px::Variant(static_cast<std::int64_t>(animationHandle));
    suite.Require(animationHandle != 0 && host.InvokeCommand(animationCommand),
                  "async command should begin an engine-owned animation wait");
    host.Update(0.05f);
    const auto animationCheckpoint = host.CapturePending();
    suite.Require(animationCheckpoint.size() == 1 &&
                      animationCheckpoint.front().waitKind == "animation" &&
                      animationCheckpoint.front().handle == animationHandle,
                  "animation checkpoint should preserve the Timeline playback handle");
    timeline.Update(0.2f);
    host.Update(0.0f);
    suite.Expect(!host.HasPendingCommand() &&
                     console.back().text == "async-animation:done",
                 "async command should resume only after Timeline playback completes");

    const auto acceptedBreakpoints = host.SetDebugBreakpoints({
        {.source = "demo.ts", .line = 10},
        {.source = "demo.ts", .line = 10},
        {.source = "demo.ts", .line = 0}});
    suite.Require(acceptedBreakpoints.size() == 1,
                  "debugger should normalize and deduplicate valid breakpoints");
    px::vn::Command debugCommand;
    debugCommand.type = "js.demo.debug";
    debugCommand.typedArgs["score"] = px::Variant(std::int64_t{4});
    suite.Require(host.InvokeCommand(debugCommand) && host.HasPendingCommand(),
                  "debug probe should suspend an async command at a breakpoint");
    const auto& breakpointState = host.CaptureDebugState();
    const auto watched = host.EvaluateDebugWatch("state.nested.value");
    suite.Require(breakpointState.paused && breakpointState.reason == "breakpoint" &&
                      breakpointState.frames.size() == 1 &&
                      breakpointState.frames.front().source ==
                          "Content/Extensions/demo.ts" &&
                      breakpointState.frames.front().line == 10 && watched &&
                      watched->value == "watch-ready" &&
                      !host.EvaluateDebugWatch("state['nested']"),
                  "breakpoint snapshot should expose source, line, locals, and safe watches");
    suite.Require(host.DebugStep(),
                  "step should resume a command paused at a debug probe");
    host.Update(0.0f);
    const auto& stepState = host.CaptureDebugState();
    const auto steppedScore = host.EvaluateDebugWatch("state.score");
    suite.Require(stepState.paused && stepState.reason == "step" &&
                      stepState.frames.front().line == 11 && steppedScore &&
                      steppedScore->value == "5",
                  "step should stop at the next probe with refreshed locals");
    suite.Require(host.DebugContinue(),
                  "continue should resume a stepped JavaScript command");
    host.Update(0.0f);
    suite.Expect(!host.HasPendingCommand() &&
                     !host.CaptureDebugState().paused &&
                     console.back().text == "debug-done:5",
                 "continued command should run to completion");
    host.SetDebugBreakpoints({});
    suite.Require(host.DebugPause() && host.InvokeCommand(debugCommand),
                  "pause request should arm the next cooperative debug probe");
    suite.Expect(host.CaptureDebugState().paused &&
                     host.CaptureDebugState().reason == "pause",
                 "pause request should report an explicit pause reason");
    host.CancelPending();

    px::ui::ActionInvocation asynchronousAction;
    asynchronousAction.action = "js.demo.asyncAction";
    asynchronousAction.arguments["seconds"] = px::Variant(0.04);
    asynchronousAction.context.sourceScene = "async-scene";
    const auto actionStart = host.StartAction(asynchronousAction);
    suite.Require(actionStart.status && actionStart.pending && actionStart.handle != 0 &&
                      host.ActionState(actionStart.handle) ==
                          px::ui::ActionExecutionState::Running,
                  "async Action should expose a pollable execution handle");
    host.Update(0.02f);
    const auto actionCheckpoint = host.CapturePendingActions();
    suite.Require(actionCheckpoint.size() == 1 &&
                      actionCheckpoint.front().id == actionStart.handle &&
                      actionCheckpoint.front().yieldIndex == 1 &&
                      std::abs(actionCheckpoint.front().remainingSeconds - 0.02f) <
                          0.001f,
                  "Action checkpoint should preserve provider handle and timer state");
    host.CancelAction(actionStart.handle);
    suite.Expect(host.ActionState(actionStart.handle) ==
                     px::ui::ActionExecutionState::Cancelled,
                 "cancelling a JavaScript Action should publish its terminal state");
    suite.Require(static_cast<bool>(host.RestorePendingActions(actionCheckpoint)) &&
                      host.ActionState(actionStart.handle) ==
                          px::ui::ActionExecutionState::Running,
                  "Action continuation should reconstruct the saved provider handle");
    host.Update(0.03f);
    const auto secondActionCheckpoint = host.CapturePendingActions();
    suite.Require(secondActionCheckpoint.size() == 1 &&
                      secondActionCheckpoint.front().yieldIndex == 2,
                  "resumed Action should advance to its second await boundary");
    host.Update(0.02f);
    suite.Expect(!host.HasPendingAction() &&
                     host.ActionState(actionStart.handle) ==
                         px::ui::ActionExecutionState::Completed &&
                     console.back().text == "async-action:done",
                 "async Action should report completion after its final wait");

    px::ui::ActionInvocation failingAction;
    failingAction.action = "js.demo.failingAction";
    const auto failingStart = host.StartAction(failingAction);
    suite.Require(failingStart.status && failingStart.pending,
                  "async Action rejection fixture should begin at an engine wait");
    host.Update(0.0f);
    suite.Expect(host.ActionState(failingStart.handle) ==
                     px::ui::ActionExecutionState::Failed,
                 "Promise rejection after resume should become a failed Action state");
    suite.Expect(!host.RunString("Engine.WaitSeconds(0.1)",
                                 "javascript-orphan-wait"),
                 "persistent waits must be scoped to an invoked async callback");

    const auto started = std::chrono::steady_clock::now();
    suite.Expect(!host.RunString("while (true) {}", "javascript-budget"),
                 "runaway JavaScript must be interrupted");
    suite.Expect(std::chrono::steady_clock::now() - started <
                     std::chrono::seconds(2),
                 "runaway JavaScript should respect the execution budget");
}

void TestPhaseThreePublicRuntimeSurfaces(px::test::Suite& suite) {
    px::test::TempDirectory temp("javascript-phase-three");
    px::io::VFS vfs;
    vfs.MountDirectory(temp.path.string());
    px::graphics::AssetCache assets(nullptr, vfs);
    px::graphics::Renderer2D renderer(nullptr, assets);
    px::audio::AudioEngine audio(vfs);
    px::Input input;
    px::animation::TimelinePlayer timeline;
    bool canonicalLoaderCalled = false;
    const auto animationId = px::Uuid::FromName("phase-three-loader");

    px::script::ScriptServices services;
    services.vfs = &vfs;
    services.renderer = &renderer;
    services.audio = &audio;
    services.input = &input;
    services.timeline = &timeline;
    services.loadAnimationAsset = [&](const std::string& path) {
        canonicalLoaderCalled = path == "Animations/show.pxtimeline";
        return px::Result<std::string>::Success(animationId.ToString());
    };
    px::script::JavaScriptHost host(services);

    input.InjectAction(px::InputAction::Advance);
    suite.Require(host.RunString(R"js(
        const resource = Engine.LoadAnimation("Animations/show.pxtimeline");
        if (!resource) throw new Error("canonical Timeline loader missing");
        const events = Engine.GetInputEvents();
        const advance = events.find((event) => event.type === "action" &&
                                               event.action === "advance");
        if (!advance || !Engine.ConsumeInputEvent(advance.id) ||
            !Engine.SuppressInputDefault(advance.id)) {
            throw new Error("consumable input event surface missing");
        }
        if (!Engine.RegisterInputAction("portrait.interact"))
            throw new Error("custom input action registration failed");
        if (!Engine.SetInputActionContext("portrait.interact", "game") ||
            !Engine.PushInputContext("game") ||
            !Engine.GetInputContexts().includes("game") ||
            !Engine.PopInputContext())
            throw new Error("input context stack is not exposed");
        if (!Engine.BindInputKey("portrait.interact", 27) ||
            Engine.GetInputKeyBindings("portrait.interact")[0] !== 27)
            throw new Error("input key inspection failed");
        if (!Engine.SetInputWheelBinding("portrait.interact", "up", 1.5) ||
            Engine.GetInputWheelBinding("portrait.interact")?.direction !== "up" ||
            Engine.GetInputWheelBinding("portrait.interact")?.threshold !== 1.5 ||
            Engine.SetInputWheelBinding("portrait.interact", "none", 1))
            throw new Error("wheel binding editing or inspection failed");
        const conflict = Engine.RebindInputKey("portrait.interact", 44);
        if (conflict.status !== "conflict" ||
            !conflict.conflicts.includes("accept") ||
            Engine.GetInputKeyBindings("portrait.interact")[0] !== 27)
            throw new Error("conflicting input rebind changed the old shortcut");
        if (Engine.GetInputKeyCode("F1") !== 58 ||
            Engine.GetInputKeyName(58) !== "F1")
            throw new Error("readable key names did not round-trip");
        const rebound = Engine.RebindInputKey("portrait.interact", "F1");
        if (rebound.status !== "applied" ||
            Engine.GetInputKeyBindings("portrait.interact")[0] !== 58)
            throw new Error("input shortcut was not replaced");
        const added = Engine.AddInputKeyBinding("portrait.interact", "F2");
        const addedKeys = Engine.GetInputKeyBindings("portrait.interact");
        if (added.status !== "applied" || addedKeys.length !== 2 ||
            addedKeys[0] !== 58 || addedKeys[1] !== 59)
            throw new Error("additive input shortcut lost its previous key");
        if (!Engine.RemoveInputKeyBinding("portrait.interact", "F1") ||
            Engine.RemoveInputKeyBinding("portrait.interact", "F1") ||
            Engine.GetInputKeyBindings("portrait.interact").join(",") !== "59")
            throw new Error("per-key removal did not keep the other shortcut");
        if (Engine.RebindInputKey("portrait.interact", "not-a-key").status !==
                "invalid-scancode" ||
            Engine.GetInputKeyBindings("portrait.interact")[0] !== 59)
            throw new Error("invalid key name changed the shortcut");
        if (!Engine.UpsertRenderNode("portrait.mouth", {
              kind: "image", phase: "after-stage", order: 4,
              path: "Assets/portrait.png",
              bounds: {x: 20, y: 20, w: 80, h: 40},
              source: {x: 0, y: 64, w: 128, h: 64},
              clip: {x: 16, y: 16, w: 96, h: 64},
              translateX: 2, scaleX: 1.1, rotation: 3
            })) throw new Error("retained render node registration failed");
        const audio = Engine.PlayAudio("Assets/missing.wav", "voice");
        if (audio <= 0 || Engine.GetAudioStatus(audio) !== "failed" ||
            Engine.GetAudioPlayback(audio)?.id !== audio) {
            throw new Error("audio handle/status surface missing");
        }
    )js", "phase-three-public-runtime"),
                  "Phase 3 public runtime surfaces should execute in the embedded host");
    suite.Expect(canonicalLoaderCalled,
                 "JavaScript animation loading should use the canonical RuntimeSession seam");
    suite.Expect(!input.ActionPressed(px::InputAction::Advance),
                 "suppressed input events should prevent the product default in the same frame");
    suite.Require(host.RunString(R"js(
        if (!Engine.BeginInputKeyCapture() ||
            !Engine.IsInputKeyCaptureActive() ||
            Engine.TakeCapturedInputKey() !== null)
            throw new Error("input key capture did not start");
    )js", "phase-three-key-capture-start"),
                  "JavaScript can start a one-shot physical key capture");
    input.InjectKeyPress(*px::Input::KeyCode("F2"));
    suite.Require(host.RunString(R"js(
        const captured = Engine.TakeCapturedInputKey();
        if (captured !== Engine.GetInputKeyCode("F2") ||
            Engine.IsInputKeyCaptureActive() ||
            Engine.TakeCapturedInputKey() !== null)
            throw new Error("captured key did not cross the runtime boundary once");
        if (!Engine.BeginInputKeyCapture())
            throw new Error("key capture could not restart");
        Engine.CancelInputKeyCapture();
        if (Engine.IsInputKeyCaptureActive())
            throw new Error("key capture did not cancel");
    )js", "phase-three-key-capture-finish"),
                  "JavaScript receives and cancels captured keys without exposing raw events");

    suite.Require(host.RunString(R"js(
        Engine.RegisterAction("phase3.invalid-audio-wait", async () => {
          await Engine.WaitAudio(999999);
          throw new Error("invalid audio wait resumed as a success");
        });
    )js", "phase-three-invalid-audio-wait"),
                  "invalid audio wait fixture should register");
    px::ui::ActionInvocation invalidAudioWait;
    invalidAudioWait.action = "phase3.invalid-audio-wait";
    const auto invalidWait = host.StartAction(invalidAudioWait);
    suite.Require(invalidWait.status && invalidWait.pending,
                  "invalid audio wait should begin as a persistent Action boundary");
    host.Update(0.0f);
    suite.Expect(host.ActionState(invalidWait.handle) ==
                     px::ui::ActionExecutionState::Failed,
                 "an evicted or nonexistent audio handle must fail rather than falsely complete");

    // Generators yield the same await tokens as async callbacks, audio included.
    suite.Require(host.RunString(R"js(
        Engine.RegisterAction("phase3.generator-audio-wait", function* () {
          yield Engine.AwaitAudio(999999);
          throw new Error("invalid audio token resumed as a success");
        });
    )js", "phase-three-generator-audio-wait"),
                  "generator audio wait fixture should register");
    px::ui::ActionInvocation generatorAudioWait;
    generatorAudioWait.action = "phase3.generator-audio-wait";
    const auto generatorWait = host.StartAction(generatorAudioWait);
    suite.Require(generatorWait.status && generatorWait.pending,
                  "a generator may yield an audio await token as a persistent boundary");
    host.Update(0.0f);
    suite.Expect(host.ActionState(generatorWait.handle) ==
                     px::ui::ActionExecutionState::Failed,
                 "a yielded nonexistent audio handle must fail rather than falsely complete");

    const auto retained = host.CaptureExtensionState();
    suite.Require(retained && retained.Value().size() == 1 &&
                      retained.Value().front().providerId ==
                          "$prismatix.render-tree",
                  "retained render nodes should enter the host checkpoint");
    suite.Require(host.RunString("Engine.ClearRenderNodes();", "clear-render") &&
                      host.CaptureExtensionState().Value().empty() &&
                      host.RestoreExtensionState(retained.Value()),
                  "retained render tree should clear and restore transactionally");
    const auto restored = host.CaptureExtensionState();
    suite.Expect(restored && restored.Value().size() == 1 &&
                     restored.Value().front().state ==
                         retained.Value().front().state,
                 "retained render checkpoint should round-trip exactly");
}

std::string VariableText(const px::vn::VariableStore& variables, const std::string_view name) {
    const auto* value = variables.GetValue(name);
    if (!value) return {};
    if (const auto* text = value->TryGet<std::string>()) return *text;
    if (const auto* integer = value->TryGet<std::int64_t>()) return std::to_string(*integer);
    if (const auto* boolean = value->TryGet<bool>()) return *boolean ? "true" : "false";
    return "<value>";
}

void TestCancellationAndEventListeners(px::test::Suite& suite) {
    px::vn::VariableStore variables;
    std::vector<px::script::ConsoleMessage> console;
    px::script::JavaScriptHost* hostPointer = nullptr;
    std::uint64_t cancelFromService = 0;
    px::script::ScriptServices services;
    services.variables = &variables;
    services.console = [&console](const px::script::ConsoleMessage& message) {
        console.push_back(message);
    };
    // A service that cancels an Action while that Action's JavaScript runs:
    // the cancellation must wait until the host call returns.
    services.advanceDialogue = [&] {
        if (hostPointer && cancelFromService != 0)
            hostPointer->CancelAction(cancelFromService,
                                      px::script::CancellationReason::Closed);
        return true;
    };
    auto host = std::make_unique<px::script::JavaScriptHost>(services);
    hostPointer = host.get();
    const auto text = [&](const std::string_view name) {
        return VariableText(variables, name);
    };
    const auto errors = [&] {
        return std::ranges::count_if(console, [](const auto& message) {
            return message.level == px::script::ConsoleLevel::Error;
        });
    };

    suite.Require(host->RunString(R"js(
        const note = (name, value) => Engine.SetVariable(name, value, "session");
        const bump = (name) => note(name, (Engine.GetVariable(name) ?? 0) + 1);
        Engine.RegisterAction("cancel.async", async (args, context) => {
            const signal = context.abortSignal;
            if (signal.aborted || signal.reason !== undefined)
                throw new Error("a fresh signal must not be aborted");
            signal.addEventListener("abort", () =>
                note(`${args.tag}.listener`, `${signal.aborted}:${signal.reason.kind}`));
            signal.onAbort((reason) => note(`${args.tag}.onAbort`, reason.name));
            try {
                await Engine.WaitSeconds(10);
                note(`${args.tag}.after`, true);
            } catch (error) {
                note(`${args.tag}.caught`, `${error.name}:${error.kind}:${error.code}`);
                throw error;
            } finally {
                bump(`${args.tag}.finally`);
                try { await Engine.WaitSeconds(1); note(`${args.tag}.rewaited`, true); }
                catch (error) { note(`${args.tag}.rewait`, error.name); }
            }
        });
        Engine.RegisterAction("cancel.generator", function* (args, context) {
            try {
                yield Engine.AwaitSeconds(10);
                note("generator.after", true);
            } finally {
                note("generator.finally", context.abortSignal.reason.kind);
            }
        });
        Engine.RegisterAction("cancel.deferred", async (args, context) => {
            await Engine.WaitSeconds(0);
            Engine.AdvanceDialogue();
            note("deferred.sameCall", context.abortSignal.aborted);
            try { await Engine.WaitSeconds(10); }
            finally { note("deferred.finally", context.abortSignal.reason?.kind ?? "none"); }
        });
        Engine.RegisterCommand("cancel.command", async (args, context) => {
            try { await Engine.WaitSeconds(args.seconds); note(`${args.tag}.done`, true); }
            finally {
                if (context.abortSignal.aborted)
                    note(`${args.tag}.cancelled`, context.abortSignal.reason.kind);
            }
        });
    )js", "cancellation-fixture"),
                  "cancellation fixture should register");

    // Reentry restart and scene close arrive through the production provider.
    px::ui::ActionCatalog catalog;
    for (const auto* id : {"cancel.async", "cancel.generator", "cancel.deferred"}) {
        px::ui::ActionDescriptor descriptor;
        descriptor.id = id;
        descriptor.label = id;
        descriptor.displayName = id;
        descriptor.category = "Test";
        descriptor.origin = px::ui::ActionOrigin::ScriptExtension;
        descriptor.providerId = "script";
        descriptor.reentryPolicy = px::ui::ActionReentryPolicy::Restart;
        descriptor.allowAdditionalArguments = true;
        suite.Require(static_cast<bool>(catalog.Register(std::move(descriptor))),
                      "cancellation Action descriptor should register");
    }
    px::ui::ActionDispatcher actions(catalog);
    suite.Require(static_cast<bool>(actions.RegisterProvider(host->CreateActionProvider())),
                  "JavaScript provider should register");
    px::ui::ActionInvocation restart;
    restart.action = "cancel.async";
    restart.arguments["tag"] = px::Variant(std::string("restart"));
    restart.context.sourceScene = "scene-a";
    const auto first = actions.Start(restart);
    suite.Require(first && host->HasPendingAction(),
                  "async Action should suspend on an engine wait");
    const auto second = actions.Start(restart);
    suite.Expect(second && actions.State(first.Value()) == px::ui::ActionExecutionState::Cancelled &&
                     text("restart.listener") == "true:restart" &&
                     text("restart.onAbort") == "AbortError" &&
                     text("restart.caught") == "AbortError:restart:PXJS7301" &&
                     text("restart.finally") == "1" && text("restart.rewait") == "AbortError" &&
                     text("restart.after").empty() && text("restart.rewaited").empty() &&
                     host->CapturePendingActions().size() == 1 && errors() == 0,
                 "restart should abort the signal, reject the pending wait and run finally before the new run starts");
    actions.CancelSource("scene-a");
    suite.Expect(!host->HasPendingAction() && text("restart.finally") == "2" &&
                     text("restart.listener") == "true:closed" && errors() == 0,
                 "closing the scene should cancel its running Action with \"closed\"");

    px::ui::ActionInvocation generator;
    generator.action = "cancel.generator";
    const auto generatorStart = host->StartAction(generator);
    suite.Require(generatorStart.status && generatorStart.pending,
                  "generator Action should suspend on a yielded token");
    host->CancelAction(generatorStart.handle, px::script::CancellationReason::Restart);
    host->Update(20.0f);
    suite.Expect(text("generator.finally") == "restart" && text("generator.after").empty() &&
                     host->ActionState(generatorStart.handle) ==
                         px::ui::ActionExecutionState::Cancelled &&
                     errors() == 0,
                 "a cancelled generator should unwind through finally and never resume");

    px::ui::ActionInvocation deferred;
    deferred.action = "cancel.deferred";
    const auto deferredStart = host->StartAction(deferred);
    suite.Require(deferredStart.status && deferredStart.pending,
                  "deferred cancellation fixture should start");
    cancelFromService = deferredStart.handle;
    host->Update(0.0f);
    cancelFromService = 0;
    suite.Expect(text("deferred.sameCall") == "false" &&
                     text("deferred.finally") == "closed" && !host->HasPendingAction() &&
                     host->ActionState(deferredStart.handle) ==
                         px::ui::ActionExecutionState::Cancelled &&
                     errors() == 0,
                 "cancellation requested while the Action runs should end it once the host call returns");

    px::vn::Command skipped;
    skipped.type = "cancel.command";
    skipped.typedArgs["seconds"] = px::Variant(5.0);
    skipped.typedArgs["tag"] = px::Variant(std::string("skip"));
    suite.Require(host->InvokeCommand(skipped) && host->HasPendingCommand(),
                  "command should hold the Story on an engine wait");
    suite.Expect(host->CancelPendingCommands(px::script::CancellationReason::Skip) &&
                     !host->HasPendingCommand() && text("skip.cancelled") == "skip" &&
                     text("skip.done").empty() &&
                     !host->CancelPendingCommands(px::script::CancellationReason::Skip),
                 "skip should cancel the waiting Story command");

    px::vn::Command loaded = skipped;
    loaded.typedArgs["seconds"] = px::Variant(0.5);
    loaded.typedArgs["tag"] = px::Variant(std::string("load"));
    suite.Require(host->InvokeCommand(loaded) && host->HasPendingCommand(),
                  "checkpoint command should start");
    const auto checkpoint = host->CapturePending();
    suite.Expect(static_cast<bool>(host->RestoreCheckpoint(checkpoint, {},
                                                           px::script::CancellationReason::Load)) &&
                     text("load.cancelled") == "load" && host->HasPendingCommand(),
                 "load should cancel the running handler, then replay the saved one with a fresh signal");
    variables.SetValue("load.cancelled", px::Variant(std::string()));
    host->Update(1.0f);
    suite.Expect(!host->HasPendingCommand() && text("load.done") == "true" &&
                     text("load.cancelled").empty() && errors() == 0,
                 "the replayed handler should complete normally");
    suite.Require(host->InvokeCommand(loaded), "rollback fixture should start");
    suite.Expect(static_cast<bool>(host->RestoreCheckpoint(host->CapturePending(), {},
                                                           std::nullopt)) &&
                     text("load.cancelled").empty() && host->HasPendingCommand(),
                 "a restore without a reason replaces handlers silently");
    host->CancelPending();
    suite.Expect(text("load.cancelled") == "reset" && !host->HasPendingCommand(),
                 "CancelPending without a reason ends handlers with \"reset\"");

    // Native listener removal.
    suite.Require(host->RunString(R"js(
        globalThis.calls = { kept: 0, removed: 0, once: 0, late: 0 };
        globalThis.kept = Engine.on("tick", () => { calls.kept++; });
        globalThis.removed = Engine.on("tick", () => { calls.removed++; });
        globalThis.onceId = Engine.on("tick", () => {
            calls.once++;
            Engine.off(onceId);
            Engine.off(removed);
            Engine.on("tick", () => { calls.late++; });
        });
        if (typeof kept !== "number" || kept === removed) throw new Error("listener ids");
    )js", "listener-fixture"),
                  "listener fixture should subscribe");
    suite.Expect(host->ListenerCount("tick") == 3, "three native listeners are registered");
    suite.Require(static_cast<bool>(host->Emit("tick")), "tick should dispatch");
    suite.Expect(host->ListenerCount("tick") == 2 &&
                     host->RunString(R"js(
        if (calls.kept !== 1 || calls.once !== 1 || calls.late !== 0)
            throw new Error(`first dispatch ${JSON.stringify(calls)}`);
        if (calls.removed !== 1 && calls.removed !== 0) throw new Error("removed");
        globalThis.removedBefore = calls.removed;
    )js", "listener-first"),
                 "a listener removed during dispatch is skipped and one added waits for the next event");
    suite.Require(static_cast<bool>(host->Emit("tick")), "second tick should dispatch");
    suite.Expect(host->RunString(R"js(
        if (calls.kept !== 2 || calls.once !== 1 || calls.late !== 1 ||
            calls.removed !== removedBefore)
            throw new Error(`second dispatch ${JSON.stringify(calls)}`);
        if (!Engine.off(kept) || Engine.off(kept) || Engine.listenerCount("tick") !== 1)
            throw new Error("off did not remove exactly once");
    )js", "listener-second") &&
                     host->ListenerCount("tick") == 1 && host->ListenerCount() == 1,
                 "Engine.off removes the native listener and listenerCount reports it");

    // Shutdown ends what is still running.
    px::ui::ActionInvocation shutdown;
    shutdown.action = "cancel.async";
    shutdown.arguments["tag"] = px::Variant(std::string("shutdown"));
    suite.Require(host->StartAction(shutdown).pending, "shutdown fixture should start");
    hostPointer = nullptr;
    host.reset();
    suite.Expect(text("shutdown.listener") == "true:shutdown" &&
                     text("shutdown.finally") == "1",
                 "destroying the host should cancel running handlers with \"shutdown\"");
}

}  // namespace

std::string ExtensionManifest(const std::string_view id, const std::string_view entry,
                              const std::string_view capabilities,
                              const std::string_view extra = {}) {
    return std::string("{\"format\":\"PrismatiXExtension\",\"schemaRevision\":1,") +
        "\"language\":\"javascript\",\"id\":\"" + std::string(id) +
        "\",\"version\":\"1.0.0\",\"requiredEngineVersion\":\"^0.1.0\"," +
        "\"entry\":\"" + std::string(entry) + "\",\"capabilities\":[" +
        std::string(capabilities) + "]," + std::string(extra) +
        "\"commands\":[],\"actions\":[]}";
}

void TestRawGraphicsBudgets(px::test::Suite& suite) {
    px::script::ScriptRawGraphics::Limits limits;
    limits.verticesPerFrame = 6;
    limits.uploadsPerFrame = 2;
    px::script::ScriptRawGraphics raw(nullptr, limits);
    const std::vector<float> triangle(3 * 8, 0.5f);
    suite.Require(!raw.CreateMesh("a", nullptr) && !raw.CreateMesh("b", nullptr) &&
                      !raw.CreateMesh("c", nullptr),
                  "raw meshes should be created");
    raw.BeginFrame();
    suite.Require(!raw.UploadVertices("a", triangle.data(), triangle.size(), std::nullopt) &&
                      !raw.UploadVertices("b", triangle.data(), triangle.size(), std::nullopt),
                  "two uploads fit the frame budget");
    const auto third = raw.UploadVertices("c", triangle.data(), triangle.size(), std::nullopt);
    suite.Expect(third && third->range && third->code == "PXJS7801",
                 "an upload past the frame budget throws PXJS7801");
    raw.BeginFrame();
    suite.Require(!raw.UploadVertices("c", triangle.data(), triangle.size(), std::nullopt),
                  "the upload budget restarts each frame");
    const auto number = [&raw](const char* key) {
        const auto stats = raw.StatsValue();
        const auto* value = stats.AsObject() ? stats.AsObject()->at(key).TryGet<std::int64_t>() : nullptr;
        return value ? *value : -1;
    };
    raw.Render(px::script::RenderPhase::AfterStage, {}, {});
    suite.Expect(number("vertices") == 6 && number("skippedMeshes") == 1,
                 "a mesh past the per-frame vertex budget is skipped, not drawn");
    raw.Render(px::script::RenderPhase::AfterUI, {}, {});
    suite.Expect(number("vertices") == 6, "later phases of the same frame share its budget");
    raw.Render(px::script::RenderPhase::AfterStage, {}, {});
    suite.Expect(number("vertices") == 6 && number("skippedMeshes") == 1,
                 "a new rendered frame restarts the draw budget");
    const auto self = raw.CreateMesh("d", nullptr);
    px::script::ScriptRawGraphics::Region region{0, 0, 2, 2};
    suite.Expect(!self && raw.CreateTexture("t", 4, 4, nullptr) == std::nullopt &&
                     raw.UploadTexture("t", nullptr, 15, region).has_value(),
                 "texture uploads must be exactly region width x height x 4 bytes");
    const px::Variant masked(px::VariantObject{{"mask", "d"}, {"target", "rt"}});
    const auto invalid = raw.CreateMesh("e", &masked);
    suite.Expect(invalid && !invalid->range,
                 "a mesh drawn into a target cannot be masked");

    // A batch is one upload, checks every part first and applies all or none.
    using Upload = px::script::ScriptRawGraphics::Upload;
    const std::vector<float> quad(4 * 8, 0.25f);
    const std::vector<std::uint16_t> quadIndices{0, 1, 2, 0, 2, 3};
    const std::vector<std::uint16_t> outside{0, 1, 4};
    raw.BeginFrame();
    suite.Require(!raw.UploadVertices("a", triangle.data(), triangle.size(), std::nullopt),
                  "a single upload fits the frame");
    const std::vector<Upload> batch{
        {.id = "b", .vertices = quad.data(), .floatCount = quad.size(), .indices16 = quadIndices.data(),
         .indexCount = quadIndices.size(), .hasVertices = true, .hasIndices = true},
        {.id = "c", .vertices = quad.data(), .floatCount = quad.size(), .indices16 = quadIndices.data(),
         .indexCount = quadIndices.size(), .hasVertices = true, .hasIndices = true},
        {.id = "d", .vertices = quad.data(), .floatCount = quad.size(), .hasVertices = true}};
    suite.Expect(!raw.UploadBatch(batch) && number("uploads") == 2,
                 "a batch over three meshes counts as one upload, and indices check against the vertices it brings");
    const std::vector<Upload> broken{
        {.id = "a", .vertices = quad.data(), .floatCount = quad.size(), .hasVertices = true},
        {.id = "d", .indices16 = outside.data(), .indexCount = outside.size(), .hasIndices = true}};
    raw.BeginFrame();
    const auto refused = raw.UploadBatch(broken);
    raw.Render(px::script::RenderPhase::AfterStage, {}, {});
    suite.Expect(refused && refused->code == "PXJS7802" && number("uploads") == 0 && number("vertices") == 3,
                 "a batch with one bad part changes nothing and is not counted");
    const std::vector<Upload> twice{
        {.id = "a", .vertices = triangle.data(), .floatCount = triangle.size(), .hasVertices = true},
        {.id = "a", .vertices = triangle.data(), .floatCount = triangle.size(), .hasVertices = true}};
    const auto duplicated = raw.UploadBatch(twice);
    suite.Expect(duplicated && !duplicated->range, "a batch names each mesh at most once");

    // Masks: a list unites coverage, maskInverted flips it, at most 16.
    const px::Variant several(px::VariantObject{
        {"mask", px::VariantArray{px::Variant("b"), px::Variant("c"), px::Variant("b")}},
        {"maskInverted", true}});
    suite.Expect(!raw.CreateMesh("f", &several), "a mesh takes a list of masks and maskInverted");
    px::VariantArray many;
    for (int index = 0; index < 17; ++index) many.emplace_back("m" + std::to_string(index));
    const px::Variant tooMany(px::VariantObject{{"mask", many}});
    const auto excess = raw.CreateMesh("g", &tooMany);
    suite.Expect(excess && excess->code == "PXJS7804", "more than 16 masks throw PXJS7804");
    const px::Variant notBoolean(px::VariantObject{{"maskInverted", "yes"}});
    const auto wrongType = raw.CreateMesh("g", &notBoolean);
    suite.Expect(wrongType && wrongType->code == "PXJS7803", "maskInverted must be a boolean");

    // Effects. Without a renderer there is no effect list: only the shape is
    // checked, and a drawn mesh counts as drawn without its effect.
    px::script::ScriptRawGraphics effects(nullptr);
    const auto effectOptions = [](px::VariantObject effect) {
        return px::Variant(px::VariantObject{{"effect", px::Variant(std::move(effect))}});
    };
    const auto vector = [](std::initializer_list<double> values) {
        px::VariantArray array;
        for (const double value : values) array.emplace_back(value);
        return px::Variant(std::move(array));
    };
    const px::Variant full = effectOptions(
        {{"id", "outline"}, {"progress", 2.0}, {"seed", 7},
         {"parameters", px::Variant(px::VariantObject{
             {"width", 2.0}, {"offset", vector({1.0, -1.0})}, {"color", vector({1.0, 0.5, 0.0, 1.0})}})}});
    const px::Variant idOnly = effectOptions({{"id", "outline"}});
    suite.Expect(!effects.CreateMesh("outlined", &full) && !effects.CreateMesh("plain", &idOnly),
                 "a mesh takes an effect id with named number, vec2 and color values, progress and seed");
    bool found = false;
    const px::Variant clearEffect(px::VariantObject{{"effect", px::Variant()}});
    suite.Expect(!effects.UpdateMesh("plain", clearEffect, found) && found, "effect: null removes the effect");
    const auto rejects = [&](const px::Variant& options) {
        const auto failure = effects.CreateMesh("refused", &options);
        return failure && !failure->range && failure->code == "PXJS7803";
    };
    px::VariantObject nine;
    for (int index = 0; index < 9; ++index) nine.emplace("u" + std::to_string(index), 1.0);
    suite.Expect(rejects(px::Variant(px::VariantObject{{"effect", "outline"}})) &&
                     rejects(effectOptions({})) && rejects(effectOptions({{"id", ""}})) &&
                     rejects(effectOptions({{"id", "outline"}, {"strength", 1.0}})) &&
                     rejects(effectOptions({{"id", "outline"}, {"progress", "half"}})) &&
                     rejects(effectOptions({{"id", "outline"},
                                            {"seed", std::numeric_limits<double>::infinity()}})) &&
                     rejects(effectOptions({{"id", "outline"}, {"parameters", "wide"}})) &&
                     rejects(effectOptions({{"id", "outline"}, {"parameters", px::Variant(nine)}})) &&
                     rejects(effectOptions({{"id", "outline"}, {"parameters", px::Variant(px::VariantObject{
                                                {"color", vector({1.0, 1.0, 1.0, 1.0, 1.0})}})}})) &&
                     rejects(effectOptions({{"id", "outline"}, {"parameters", px::Variant(px::VariantObject{
                                                {"width", "2"}})}})),
                 "an effect needs an id and takes only finite numbers or 1 to 4 of them as values");
    const px::Variant intoTarget(px::VariantObject{
        {"target", "rt"}, {"effect", px::Variant(px::VariantObject{{"id", "outline"}})}});
    const auto onTarget = effects.CreateMesh("into", &intoTarget);
    const px::Variant toTarget(px::VariantObject{{"target", "rt"}});
    const auto movedToTarget = effects.UpdateMesh("outlined", toTarget, found);
    suite.Expect(onTarget && onTarget->code == "PXJS7803" && movedToTarget &&
                     movedToTarget->code == "PXJS7803",
                 "effects apply to screen meshes, not meshes drawn into a render target");
    suite.Require(!effects.UploadVertices("outlined", triangle.data(), triangle.size(), std::nullopt) &&
                      !effects.UploadVertices("plain", triangle.data(), triangle.size(), std::nullopt),
                  "the effect meshes take vertices");
    effects.Render(px::script::RenderPhase::AfterStage, {}, {});
    const auto stats = effects.StatsValue();
    const auto limitsValue = effects.LimitsValue();
    const auto* skipped = stats.AsObject()->at("skippedEffects").TryGet<std::int64_t>();
    const auto* budget = limitsValue.AsObject()->at("maxEffectsPerFrame").TryGet<std::int64_t>();
    suite.Expect(skipped && *skipped == 1 && budget && *budget == 32,
                 "a mesh drawn without its effect counts in skippedEffects; limits report maxEffectsPerFrame");
}

void TestNativeSignatureInterop(px::test::Suite& suite) {
    // Signed by Node.js the way `prismatix extension pack` signs (ECDSA P-256,
    // SHA-256, ieee-p1363); the descriptor is the SDK's nativeSignatureMessage.
    px::native::NativeLibrarySpec spec;
    spec.extensionId = "glow";
    spec.version = "1.2.0";
    spec.platform = "win32-x64";
    spec.abi = 1;
    spec.services = {"glow.b", "glow.a"};
    std::string digest;
    for (int index = 0; index < 32; ++index) digest += "ab";
    spec.files = {{"native/win32-x64/glow.dll", digest}};
    const std::string publicKey =
        "04a89c77a91034f5c232a2e3b1e71474fc7aaa14f71312e9b796419839aa76d195b11cf5f4fd186f56246fc1abc82472b20a5213e813d6d805b5654693de3d4372";
    const std::string signature =
        "b8450e2a93c9a1d96dde5a949deb5e61f1bd3ce1f88656f59976813b5a821d154551e0105378880c97083083d449810c6a6c04a9b1fc9c46901ffe49ee9d1e58";
    suite.Expect(px::native::SignatureMessage(spec) ==
                     "PrismatiX native plugin v1\nid glow\nversion 1.2.0\nplatform win32-x64\nabi 1\n"
                     "services glow.a,glow.b\nfile " + spec.files[0].sha256 + " native/win32-x64/glow.dll\n",
                 "the Player builds the same descriptor as the SDK");
    suite.Expect(px::io::VerifyEcdsaP256Signature(px::native::SignatureMessage(spec), signature, publicKey),
                 "a signature made by Node.js verifies in the Player");
    spec.version = "1.2.1";
    suite.Expect(!px::io::VerifyEcdsaP256Signature(px::native::SignatureMessage(spec), signature, publicKey),
                 "the signature binds the version");
}

void TestRawAndNativeExtensions(px::test::Suite& suite) {
    px::test::TempDirectory temp("javascript-raw-native");
    const auto extensions = temp.path / "Content" / "Extensions";
    std::filesystem::create_directories(extensions);
    const auto safety = std::string_view(
        "\"safety\":{\"previewSafe\":true,\"deterministic\":true,\"seekSafe\":true,\"rollbackSafe\":true},");
    Write(extensions / "raw-denied.js", R"js(
        try { EngineRaw.CreateMesh("x"); Engine.SetVariable("raw.denied", "allowed", "session"); }
        catch (error) { Engine.SetVariable("raw.denied", `${error.name}:${error.code}`, "session"); }
    )js");
    Write(extensions / "raw-denied.pxextension",
          ExtensionManifest("raw-denied", "raw-denied.js", "\"runtime\"", safety));
    Write(extensions / "raw.js", R"js(
        const note = (name, value) => Engine.SetVariable(name, value, "session");
        const code = (run) => { try { run(); return "ok"; } catch (error) { return `${error.name}:${error.code}`; } };
        EngineRaw.CreateMesh("quad", { blend: "additive", order: 2, phase: "after-ui" });
        EngineRaw.UploadVertices("quad", new Float32Array([
            0, 0, 0, 0, 1, 1, 1, 1,   10, 0, 1, 0, 1, 1, 1, 1,
            10, 10, 1, 1, 1, 1, 1, 1,   0, 10, 0, 1, 1, 1, 1, 1]));
        EngineRaw.UploadIndices("quad", new Uint16Array([0, 1, 2, 0, 2, 3]));
        note("raw.badIndex", code(() => EngineRaw.UploadIndices("quad", new Uint16Array([0, 1, 9]))));
        note("raw.badVertices", code(() => EngineRaw.UploadVertices("quad", new Float32Array(7))));
        note("raw.unknown", code(() => EngineRaw.UploadVertices("ghost", new Float32Array(8))));
        note("raw.badBlend", code(() => EngineRaw.UpdateMesh("quad", { blend: "screen" })));
        EngineRaw.CreateTexture("noise", 4, 4, { filter: "nearest" });
        EngineRaw.UploadTexture("noise", new Uint8Array(64));
        EngineRaw.UploadTexture("noise", new Uint8Array(16), { x: 2, y: 2, w: 2, h: 2 });
        note("raw.badPixels", code(() => EngineRaw.UploadTexture("noise", new Uint8Array(10))));
        EngineRaw.CreateTarget("glow", 64, 32, { clear: { r: 0, g: 0, b: 0, a: 0 } });
        EngineRaw.CreateMesh("into", { target: "glow", texture: { kind: "texture", id: "noise" } });
        note("raw.selfSample", code(() => EngineRaw.UpdateMesh("into", { texture: { kind: "target", id: "glow" } })));
        EngineRaw.CreateMesh("pair", { mask: ["quad", "into"], maskInverted: true });
        const square = new Float32Array(32);
        note("raw.geometry", code(() => EngineRaw.UploadGeometry("pair", square, new Uint32Array([0, 1, 2, 0, 2, 3]))));
        const before = EngineRaw.Stats().uploads;
        note("raw.batch", code(() => EngineRaw.UploadBatch([
            { mesh: "quad", vertices: square.subarray(0, 8), firstVertex: 3 },
            { mesh: "pair", vertices: square.subarray(0, 24), indices: new Uint16Array([0, 1, 2]) }])));
        note("raw.batchCount", String(EngineRaw.Stats().uploads - before));
        note("raw.batchTwice", code(() => EngineRaw.UploadBatch([{ mesh: "pair", indices: new Uint16Array(3) },
                                                                  { mesh: "pair", indices: new Uint16Array(3) }])));
        note("raw.batchShape", code(() => EngineRaw.UploadBatch([{ mesh: "pair", vertices: [0, 0] }])));
        note("raw.batchFirst", code(() => EngineRaw.UploadBatch([{ mesh: "pair", firstVertex: 1, indices: new Uint16Array(3) }])));
        const limits = EngineRaw.Limits();
        let budget = "ok";
        for (let index = 0; index < limits.maxUploadsPerFrame + 2 && budget === "ok"; index += 1)
            budget = code(() => EngineRaw.UploadIndices("quad", new Uint16Array([0, 1, 2])));
        note("raw.budget", budget);
        const stats = EngineRaw.Stats();
        note("raw.stats", `${stats.meshes}/${stats.textures}/${stats.targets}/${stats.uploads === limits.maxUploadsPerFrame}`);
        EngineRaw.CreateMesh("outlined", { texture: { kind: "target", id: "glow" }, blend: "premultiplied",
            effect: { id: "outline", parameters: { width: 2, color: [1, 0.5, 0, 1] }, progress: 0.5 } });
        note("raw.effect", code(() => EngineRaw.UpdateMesh("outlined", {
            effect: { id: "outline", parameters: { offset: [1, -1] }, progress: 1, seed: 3 } })));
        note("raw.effectClear", code(() => EngineRaw.UpdateMesh("outlined", { effect: null })));
        note("raw.effectTarget", code(() => EngineRaw.UpdateMesh("into", { effect: { id: "outline" } })));
        note("raw.effectShape", code(() => EngineRaw.UpdateMesh("outlined", { effect: { id: "outline", parameters: { width: "2" } } })));
        note("raw.effectKey", code(() => EngineRaw.CreateMesh("glowing", { effect: { id: "outline", strength: 1 } })));
        note("raw.effectStats", `${EngineRaw.Stats().skippedEffects}/${EngineRaw.Limits().maxEffectsPerFrame}`);
        note("raw.random", code(() => Math.random()));
        note("raw.frozen", code(() => { "use strict"; EngineRaw.CreateMesh = null; }));
    )js");
    Write(extensions / "raw.pxextension",
          ExtensionManifest("raw", "raw.js", "\"runtime\",\"raw.mesh\",\"raw.texture\",\"raw.target\"", safety));

    {
        std::ofstream binary(extensions / "blob.bin", std::ios::binary);
        const char bytes[]{'\x00', '\xff', '\x10', 'A'};
        binary.write(bytes, sizeof bytes);
    }
    Write(extensions / "random.js", R"js(
        const note = (name, value) => Engine.SetVariable(name, value, "session");
        const draws = [Engine.Random(), Engine.Random(), Engine.Random()];
        note("random.first", String(draws[0]));
        note("random.range", String(draws.every((value) => value >= 0 && value < 1) && new Set(draws).size === 3));
        note("random.bytes", Array.from(Engine.ReadResourceBytes("Content/Extensions/blob.bin")).join(","));
        note("random.isBytes", String(Engine.ReadResourceBytes("Content/Extensions/blob.bin") instanceof Uint8Array));
        Engine.on("draw", () => note("random.next", String(Engine.Random())));
    )js");
    Write(extensions / "random.pxextension",
          ExtensionManifest("random-demo", "random.js", "\"runtime\"", safety));

    px::io::VFS vfs;
    vfs.MountDirectory(temp.path.string());
    px::vn::VariableStore variables;
    std::vector<px::script::ConsoleMessage> console;
    px::script::ScriptServices services;
    services.vfs = &vfs;
    services.variables = &variables;
    services.console = [&console](const px::script::ConsoleMessage& message) { console.push_back(message); };

    {
        // ctx.random: seeded per session (fixed here), saved, restored.
        auto seeded = services;
        seeded.randomSeed = 42;
        std::string first;
        {
            px::script::JavaScriptHost host(seeded);
            suite.Require(host.LoadExtensionManifest("Content/Extensions/random.pxextension"),
                          "a random extension loads");
            first = VariableText(variables, "random.first");
            suite.Expect(VariableText(variables, "random.range") == "true",
                         "Engine.Random draws distinct numbers in [0, 1)");
            suite.Expect(VariableText(variables, "random.bytes") == "0,255,16,65" &&
                             VariableText(variables, "random.isBytes") == "true",
                         "Engine.ReadResourceBytes returns a packaged file's bytes as a Uint8Array");
            const auto saved = host.CaptureExtensionState();
            suite.Require(saved && saved.Value().size() == 1 &&
                              saved.Value()[0].providerId == "$prismatix.random",
                          "a used generator is part of the saved extension state");
            suite.Require(static_cast<bool>(host.Emit("draw")), "the draw listener runs");
            const auto next = VariableText(variables, "random.next");
            suite.Require(static_cast<bool>(host.Emit("draw")), "the draw listener runs again");
            suite.Expect(VariableText(variables, "random.next") != next, "the generator moves on");
            suite.Require(static_cast<bool>(host.RestoreExtensionState(saved.Value())),
                          "the saved generator restores");
            suite.Require(static_cast<bool>(host.Emit("draw")), "the draw listener runs after the restore");
            suite.Expect(VariableText(variables, "random.next") == next,
                         "after a load or rollback the same numbers follow");
            px::script::ExtensionStates corrupt = saved.Value();
            corrupt[0].state = px::Variant(px::VariantObject{{"state", std::string("not hex")}});
            suite.Expect(!host.RestoreExtensionState(corrupt), "a damaged generator state is refused");
            suite.Require(static_cast<bool>(host.RestoreExtensionState({})),
                          "a checkpoint from before the first draw restores");
            const auto unused = host.CaptureExtensionState();
            suite.Expect(unused && unused.Value().empty(),
                         "restoring a checkpoint without the generator returns it to unused");
        }
        {
            px::script::JavaScriptHost host(seeded);
            suite.Require(host.LoadExtensionManifest("Content/Extensions/random.pxextension"),
                          "the random extension loads again");
            suite.Expect(VariableText(variables, "random.first") == first,
                         "the same session seed gives the same numbers");
        }
    }

    // Layer 2: the test library, signed for this platform.
    const auto platform = std::string(px::native::CurrentPlatform());
    const bool native = !platform.empty();
    if (native) {
        const std::filesystem::path library(PRISMATIX_TEST_NATIVE_PLUGIN);
        const auto name = library.filename().string();
        const auto plugins = temp.path / "Plugins" / "native-demo";
        std::filesystem::create_directories(plugins);
        std::filesystem::copy_file(library, plugins / name);
        std::ifstream stream(plugins / name, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        const auto key = px::io::GenerateOverlaySigningKeyPair();
        suite.Require(key.has_value(), "a P-256 key should be generated");
        px::native::NativeLibrarySpec spec;
        spec.extensionId = "native-demo";
        spec.version = "1.0.0";
        spec.platform = platform;
        spec.abi = 1;
        spec.services = {"test.fail", "test.echo", "test.explain", "test.where"};
        spec.files = {{"native/" + platform + "/" + name, px::crypto::Sha256Hex(bytes)}};
        const auto signature = px::io::SignEcdsaP256(px::native::SignatureMessage(spec), key->privateKeyHex);
        suite.Require(signature.has_value(), "the library descriptor should be signed");
        const auto nativeSection = [&](const std::string& signed_) {
            return "\"native\":{\"abi\":1,\"services\":[\"test.echo\",\"test.explain\",\"test.fail\",\"test.where\"],\"publicKey\":\"" +
                key->publicKeyHex + "\",\"libraries\":{\"" + platform + "\":{\"path\":\"" + spec.files[0].path +
                "\",\"sha256\":\"" + spec.files[0].sha256 + "\",\"signature\":\"" + signed_ + "\"}}},";
        };
        Write(extensions / "native-demo.js", R"js(
            const note = (name, value) => Engine.SetVariable(name, value, "session");
            const code = (run) => { try { run(); return "ok"; } catch (error) { return `${error.name}:${error.code}`; } };
            note("native.has", EngineNative.Has("test.echo"));
            note("native.services", EngineNative.Services().join(","));
            note("native.echo", Array.from(EngineNative.Request("test.echo", new Uint8Array([1, 2, 3]))).join(","));
            note("native.fail", code(() => EngineNative.Request("test.fail", new Uint8Array(0))));
            note("native.undeclared", code(() => EngineNative.Request("test.undeclared", new Uint8Array(0))));
            try { EngineNative.Request("test.explain", new Uint8Array(0)); note("native.explain", "ok"); }
            catch (error) { note("native.explain", `${error.code}:${error.message}`); }
            note("native.where", String.fromCharCode(...EngineNative.Request("test.where", new Uint8Array(0))));
            note("native.messages", EngineNative.TakeMessages("test.echo").map((bytes) => String.fromCharCode(...bytes)).join(","));
        )js");
        Write(extensions / "native-demo.pxextension",
              ExtensionManifest("native-demo", "native-demo.js", "\"runtime\",\"native\"",
                                std::string(safety) + nativeSection(*signature)));
        // The same library under a manifest whose signature does not verify.
        std::string forged = *signature;
        forged[0] = forged[0] == '0' ? '1' : '0';
        Write(extensions / "native-forged.js", R"js(
            Engine.SetVariable("forged.has", EngineNative.Has("test.echo"), "session");
            try { EngineNative.Request("test.echo", new Uint8Array(1)); Engine.SetVariable("forged.request", "ok", "session"); }
            catch (error) { Engine.SetVariable("forged.request", error.code, "session"); }
        )js");
        auto forgedManifest = ExtensionManifest("native-demo", "native-forged.js", "\"runtime\",\"native\"",
                                                std::string(safety) + nativeSection(forged));
        Write(extensions / "native-forged.pxextension", forgedManifest);
        services.nativePluginDirectories = {(temp.path / "Plugins").string()};
    }

    {
        px::script::JavaScriptHost host(services);
        suite.Require(host.LoadExtensionManifest("Content/Extensions/raw-denied.pxextension"),
                      "an extension without raw capabilities still loads");
        suite.Expect(VariableText(variables, "raw.denied") == "PermissionError:PXJS7101",
                     "EngineRaw calls without a raw.* capability are refused (PXJS7101)");
        suite.Require(host.LoadExtensionManifest("Content/Extensions/raw.pxextension"),
                      "a raw extension loads");
        suite.Expect(VariableText(variables, "raw.badIndex") == "RangeError:PXJS7802" &&
                         VariableText(variables, "raw.badVertices") == "TypeError:PXJS7803" &&
                         VariableText(variables, "raw.unknown") == "RangeError:PXJS7802" &&
                         VariableText(variables, "raw.badBlend") == "TypeError:PXJS7803" &&
                         VariableText(variables, "raw.badPixels") == "TypeError:PXJS7803" &&
                         VariableText(variables, "raw.selfSample") == "TypeError:PXJS7803",
                     "raw arguments are checked: indices in range, 8 floats per vertex, RGBA8 sizes, blend names, no self-sampling targets");
        suite.Expect(VariableText(variables, "raw.budget") == "RangeError:PXJS7801",
                     "uploads past the per-frame budget throw PXJS7801");
        suite.Expect(VariableText(variables, "raw.geometry") == "ok" &&
                         VariableText(variables, "raw.batch") == "ok" &&
                         VariableText(variables, "raw.batchCount") == "1",
                     "UploadGeometry and UploadBatch upload vertices and indices together, a batch as one upload");
        suite.Expect(VariableText(variables, "raw.batchTwice") == "TypeError:PXJS7803" &&
                         VariableText(variables, "raw.batchShape") == "TypeError:PXJS7803" &&
                         VariableText(variables, "raw.batchFirst") == "TypeError:PXJS7803",
                     "a batch refuses a mesh named twice, plain arrays and firstVertex without vertices");
        suite.Expect(VariableText(variables, "raw.stats") == "3/1/1/true",
                     "Stats reports the extension's meshes, textures, targets and uploads");
        suite.Expect(VariableText(variables, "raw.effect") == "ok" &&
                         VariableText(variables, "raw.effectClear") == "ok" &&
                         VariableText(variables, "raw.effectStats") == "0/32",
                     "a mesh takes an effect and changes or clears it with UpdateMesh; Stats and Limits report effects");
        suite.Expect(VariableText(variables, "raw.effectTarget") == "TypeError:PXJS7803" &&
                         VariableText(variables, "raw.effectShape") == "TypeError:PXJS7803" &&
                         VariableText(variables, "raw.effectKey") == "TypeError:PXJS7803",
                     "an effect on a render target mesh, a non-numeric value or an unknown key throws PXJS7803");
        suite.Expect(VariableText(variables, "raw.frozen") == "TypeError:undefined",
                     "EngineRaw is frozen");
        suite.Expect(VariableText(variables, "raw.random") == "Error:undefined",
                     "Math.random stays disabled");
        const auto state = host.CaptureExtensionState();
        suite.Expect(state && state.Value().empty(),
                     "raw resources are not part of saved extension state");
    }
    if (!native) return;
    {
        px::script::JavaScriptHost host(services);
        suite.Require(host.LoadExtensionManifest("Content/Extensions/native-demo.pxextension"),
                      "a signed native extension loads");
        suite.Expect(VariableText(variables, "native.has") == "true" &&
                         VariableText(variables, "native.services") == "test.echo,test.explain,test.fail,test.where",
                     "the library registers its declared services");
        suite.Expect(VariableText(variables, "native.echo") == "3,2,1",
                     "a request carries bytes to the library and its response back");
        suite.Expect(VariableText(variables, "native.fail") == "RangeError:PXJS7902" &&
                         VariableText(variables, "native.undeclared") == "RangeError:PXJS7901",
                     "a failing service throws PXJS7902 and an undeclared one PXJS7901");
        suite.Expect(VariableText(variables, "native.messages") == "hi",
                     "messages the library posts reach the script");
        suite.Expect(VariableText(variables, "native.explain") ==
                         "PXJS7902:The native service 'test.explain' failed: no light in the scene",
                     "respond_error carries the library's message to the script");
        const auto where = VariableText(variables, "native.where");
        std::error_code same;
        suite.Expect(std::filesystem::equivalent(std::filesystem::path(reinterpret_cast<const char8_t*>(where.c_str())),
                                                 temp.path / "Plugins" / "native-demo", same),
                     "plugin_directory names the folder holding the library");
    }
    {
        px::script::JavaScriptHost host(services);
        suite.Require(host.LoadExtensionManifest("Content/Extensions/native-forged.pxextension"),
                      "a native extension whose signature fails still loads its script");
        suite.Expect(VariableText(variables, "forged.has") == "false" &&
                         VariableText(variables, "forged.request") == "PXJS7901",
                     "a library whose signature does not verify is never loaded");
    }
    {
        // Android's path: the library comes out of the package (here a
        // folder standing in for the APK's assets) into a private folder,
        // replacing a stale copy, and is checked and loaded there.
        const std::filesystem::path library(PRISMATIX_TEST_NATIVE_PLUGIN);
        const auto name = library.filename();
        const auto bundled = temp.path / "Bundled";
        const auto copies = temp.path / "CodeCache";
        std::filesystem::create_directories(bundled / "native-demo");
        std::filesystem::copy_file(library, bundled / "native-demo" / name);
        std::filesystem::create_directories(copies / "native-demo");
        Write(copies / "native-demo" / name, "a stale copy");
        auto copying = services;
        copying.nativePluginDirectories = {copies.string()};
        copying.nativePluginAssets = bundled.string();
        {
            px::script::JavaScriptHost host(copying);
            suite.Require(host.LoadExtensionManifest("Content/Extensions/native-demo.pxextension"),
                          "a native extension loads from copied files");
            suite.Expect(VariableText(variables, "native.echo") == "3,2,1",
                         "the library copied out of the package answers");
            const auto where = VariableText(variables, "native.where");
            std::error_code same;
            suite.Expect(std::filesystem::equivalent(std::filesystem::path(reinterpret_cast<const char8_t*>(where.c_str())),
                                                     copies / "native-demo", same),
                         "plugin_directory names the folder the files were copied to");
        }
        std::ifstream original(library, std::ios::binary);
        std::ifstream copied(copies / "native-demo" / name, std::ios::binary);
        suite.Expect(std::string((std::istreambuf_iterator<char>(original)), std::istreambuf_iterator<char>()) ==
                         std::string((std::istreambuf_iterator<char>(copied)), std::istreambuf_iterator<char>()),
                     "a stale copy is replaced by the package's bytes");
        copied.close();
        std::filesystem::remove(bundled / "native-demo" / name);
        {
            // The script runs (and then throws at its first request).
            px::script::JavaScriptHost host(copying);
            (void)host.LoadExtensionManifest("Content/Extensions/native-demo.pxextension");
            suite.Expect(VariableText(variables, "native.has") == "false",
                         "a library missing from the package is not loaded from an old copy");
        }
    }
}

int main() {
    px::test::Suite suite("JavaScriptHostIntegration");
    suite.Run("RawGraphicsBudgets", [&] { TestRawGraphicsBudgets(suite); });
    suite.Run("RawAndNativeExtensions", [&] { TestRawAndNativeExtensions(suite); });
    suite.Run("NativeSignatureInterop", [&] { TestNativeSignatureInterop(suite); });
    suite.Run("SandboxAndImmediateParity", [&] {
        TestSandboxAndImmediateParity(suite);
    });
    suite.Run("PhaseThreePublicRuntimeSurfaces", [&] {
        TestPhaseThreePublicRuntimeSurfaces(suite);
    });
    suite.Run("CancellationAndEventListeners", [&] {
        TestCancellationAndEventListeners(suite);
    });
    return suite.Finish();
}
