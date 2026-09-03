#include "Engine/Animation/Timeline.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/IO/VFS.h"
#include "Engine/Progression/GlobalProfile.h"
#include "Engine/Script/JavaScriptHost.h"
#include "Engine/UI/Actions/ActionCatalog.h"
#include "Engine/UI/Actions/ActionDispatcher.h"
#include "Engine/UI/Control.h"
#include "Engine/UI/UIRouter.h"
#include "Engine/VN/Commands/CommandRegistry.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Engine/VN/Runtime/VariableStore.h"
#include "Tests/TestSupport/TestHarness.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
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
        "schemaRevision": 2,
        "language": "javascript",
        "id": "js-demo",
        "version": "1.0.0",
        "requiredEngineVersion": ">=0.2.0 <0.3.0",
        "entry": "demo.js",
        "capabilities": ["runtime", "ui"],
        "safety": {
            "previewSafe": true,
            "deterministic": true,
            "seekSafe": false,
            "rollbackSafe": false
        },
        "commands": [
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
        "schemaRevision":2,
        "language":"javascript",
        "id":"denied",
        "version":"1.0.0",
        "requiredEngineVersion":"^0.2.0",
        "entry":"demo.js",
        "capabilities":["filesystem"],
        "commands":[],
        "actions":[]
    })json");
    Write(extensions / "incompatible.js",
          "Engine.SetVariable('version.executed', true, 'session');\n");
    Write(extensions / "incompatible.pxextension", R"json({
        "format":"PrismatiXExtension","schemaRevision":2,
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
        "format":"PrismatiXExtension","schemaRevision":2,
        "language":"javascript","id":"capability","version":"1.0.0",
        "requiredEngineVersion":"^0.2.0","entry":"capability.js",
        "capabilities":["ui"],
        "safety":{"previewSafe":true,"deterministic":true,"seekSafe":true,"rollbackSafe":true},
        "commands":[],"actions":[{"id":"capability.denied","parameters":[]}]
    })json");
    Write(extensions / "ghost.js", R"js(
        Engine.RegisterCommand("ghost.command", () => {});
        throw new Error("load must roll back");
    )js");
    Write(extensions / "ghost.pxextension", R"json({
        "format":"PrismatiXExtension","schemaRevision":2,
        "language":"javascript","id":"ghost","version":"1.0.0",
        "requiredEngineVersion":"^0.2.0","entry":"ghost.js",
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
        return std::string("{\"format\":\"PrismatiXExtension\",\"schemaRevision\":2,") +
            "\"language\":\"javascript\",\"id\":\"" + std::string(id) +
            "\",\"version\":\"1.0.0\",\"requiredEngineVersion\":\"^0.2.0\"," +
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
        "format":"PrismatiXExtension","schemaRevision":2,
        "language":"javascript","id":"module","version":"1.0.0",
        "requiredEngineVersion":"^0.2.0","entry":"module-entry.js",
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
        "format":"PrismatiXExtension","schemaRevision":2,
        "language":"javascript","id":"bad-module","version":"1.0.0",
        "requiredEngineVersion":"^0.2.0","entry":"bad-module-entry.js",
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
        "format":"PrismatiXExtension","schemaRevision":2,
        "language":"javascript","id":"traversal","version":"1.0.0",
        "requiredEngineVersion":"^0.2.0","entry":"traversal-entry.js",
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
    services.console = [&console](const px::script::ConsoleMessage& message) {
        console.push_back(message);
    };
    px::script::JavaScriptHost host(services);
    suite.Equal("javascript", host.BackendId(),
                "embedded host must identify the JavaScript backend");
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

}  // namespace

int main() {
    px::test::Suite suite("JavaScriptHostIntegration");
    suite.Run("SandboxAndImmediateParity", [&] {
        TestSandboxAndImmediateParity(suite);
    });
    return suite.Finish();
}
