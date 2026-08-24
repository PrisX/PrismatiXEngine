#include "Engine/IO/VFS.h"
#include "Engine/Progression/GlobalProfile.h"
#include "Engine/Script/JavaScriptHost.h"
#include "Engine/UI/Actions/ActionCatalog.h"
#include "Engine/UI/Actions/ActionDispatcher.h"
#include "Engine/UI/Control.h"
#include "Engine/UI/UIRouter.h"
#include "Engine/VN/Commands/CommandRegistry.h"
#include "Engine/VN/Runtime/VariableStore.h"
#include "Tests/TestSupport/TestHarness.h"

#include <chrono>
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
        Engine.RegisterAction("js.demo.action", (args, context) => {
            Engine.log(`${args.message}:${context.preview}`);
        });
        Engine.On("js.demo.event", (payload) => {
            Engine.log(`event:${payload.value}`);
        });
    )js");
    Write(extensions / "default.pxextension", R"json({
        "format": "PrismatiXExtension",
        "schemaRevision": 1,
        "language": "javascript",
        "id": "js-demo",
        "entry": "demo.js",
        "capabilities": ["runtime", "ui"],
        "commands": [{
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
        }],
        "actions": [{
            "id": "js.demo.action",
            "displayName": "JavaScript Action",
            "category": "Tests",
            "reentry": "restart",
            "parameters": [{"name":"message","type":"string","required":true}]
        }]
    })json");
    Write(extensions / "denied.pxextension", R"json({
        "format":"PrismatiXExtension",
        "schemaRevision":1,
        "language":"javascript",
        "id":"denied",
        "entry":"demo.js",
        "capabilities":["filesystem"],
        "commands":[],
        "actions":[]
    })json");

    px::io::VFS vfs;
    vfs.MountDirectory(temp.path.string());
    suite.Require(vfs.Exists("Content/Extensions/default.pxextension"),
                  "fixture directory should mount through the production VFS");
    std::vector<px::script::ConsoleMessage> console;
    px::vn::VariableStore variables;
    px::progress::GlobalProfile profile;
    px::ui::UIRouter routes;
    const auto routeFactory = []() -> px::Result<std::unique_ptr<px::ui::Control>> {
        return px::Result<std::unique_ptr<px::ui::Control>>::Success(
            std::make_unique<px::ui::Control>());
    };
    suite.Require(static_cast<bool>(routes.Register("title", routeFactory)),
                  "runtime route fixture should register");
    px::script::ScriptServices services;
    services.vfs = &vfs;
    services.variables = &variables;
    services.profile = &profile;
    services.routes = &routes;
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
        Engine.SetVariable("js.runtime", { score: 7, flags: [true, "ready"] }, "save");
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
        Engine.log("runtime-context-ready");
    )js", "javascript-runtime-context"),
                 "JavaScript RuntimeContext should bridge typed engine services");
    const auto* runtimeValue = variables.GetValue("js.runtime");
    suite.Expect(runtimeValue && runtimeValue->AsObject() &&
                     profile.HasSeen("js.scene") && profile.CGUnlocked("js.cg") &&
                     routes.CurrentRoute() == "title" &&
                     console.back().text == "runtime-context-ready",
                 "RuntimeContext mutations should reach the C++ runtime services");
    suite.Expect(!host.LoadExtensionManifest(
                     "Content/Extensions/denied.pxextension"),
                 "undeclared host capabilities must fail closed");
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
    host.Emit("js.demo.event", {{"value", "ok"}});

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
    suite.Expect(console.size() >= 5 &&
                     console[console.size() - 3].text == "hello:3" &&
                     console[console.size() - 2].text == "event:ok" &&
                     console.back().text == "action:true",
                 "command, event, and Action callbacks should receive runtime values");

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
