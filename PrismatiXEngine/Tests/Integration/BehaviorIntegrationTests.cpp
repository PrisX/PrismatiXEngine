#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "Engine/Animation/Timeline.h"
#include "Engine/Core/TypeRegistry.h"
#include "Engine/IO/Archive.h"
#include "Engine/IO/VFS.h"
#include "Engine/Lua/LuaHost.h"
#include "Engine/Platform/Input.h"
#include "Engine/Progression/Persist.h"
#include "Engine/Progression/SaveSystem.h"
#include "Engine/Resources/AssetRegistry.h"
#include "Engine/Text/Typography.h"
#include "Engine/UI/Actions/ActionCatalog.h"
#include "Engine/UI/Animation.h"
#include "Engine/UI/Behavior/BehaviorGraph.h"
#include "Engine/UI/GalgameUI.h"
#include "Engine/UI/Styles/StyleResolver.h"
#include "Engine/UI/Styles/StyleSerialization.h"
#include "Engine/UI/UIRouter.h"
#include "Engine/UI/UISceneLoader.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Engine/UI/Widgets.h"
#include "Engine/VN/Commands/CommandRegistry.h"
#include "Engine/VN/Expression/Expression.h"
#include "Engine/VN/GameCatalog.h"
#include "Engine/VN/Runtime/Dialogue.h"
#include "Engine/VN/Runtime/VariableStore.h"
#include "Engine/VN/Scenario/ScenarioDocument.h"
#include "Engine/VN/Scenario/StoryMap.h"
#include "Tests/TestSupport/TestHarness.h"

namespace {

int g_failures = 0;
std::string_view g_currentTest = "runtime integration setup";

void Check(bool condition, const char* message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAIL [" << g_currentTest << "]\n"
              << "  Expected: " << message << '\n'
              << "  Actual: predicate evaluated false\n";
}
void Check(const px::Status& status, const char* message) { Check(static_cast<bool>(status), message); }

void Run(const std::string_view name, void (*test)()) {
    g_currentTest = name;
    try {
        test();
    } catch (const std::exception& error) {
        ++g_failures;
        std::cerr << "UNCAUGHT [" << name << "]: " << error.what() << '\n';
    } catch (...) {
        ++g_failures;
        std::cerr << "UNCAUGHT [" << name << "]: unknown exception\n";
    }
}


void TestRouteStackAndModalState() {
    px::ui::RouteTable table;
    Check(static_cast<bool>(table.Register({ "title", px::resource::ResourceRef<px::ui::UISceneResourceTag>(px::Uuid::FromName("ui-title"), "Content/UI/Title.pxscene") })), "typed route table should accept a ResourceId-backed scene");
    Check(table.Find("title") && table.Find("title")->scene.LastKnownPath().ends_with("Title.pxscene"), "route table should resolve stable route ids");

    px::ui::UIRouter router;
    const auto factory = []() -> px::Result<std::unique_ptr<px::ui::Control>> { return px::Result<std::unique_ptr<px::ui::Control>>::Success(std::make_unique<px::ui::Control>()); };
    Check(router.Register("title", factory) && router.Register("game", factory) && router.Register("confirm", factory), "router factories should register");
    Check(router.Push("title") && router.Push("game") && router.ShowModal("confirm") && router.ShowModal("confirm"), "router should support push and a true modal stack");
    const auto captured = router.CaptureState();
    Check(captured.stack.size() == 2 && captured.modals.size() == 2, "router state should capture route and modal stacks");
    Check(router.CloseModal() && router.CurrentModalRoute() == "confirm", "closing one modal should reveal the previous modal");
    Check(router.RestoreState(captured) && router.CurrentRoute() == "game" && router.CurrentModalRoute() == "confirm", "router stack should restore atomically from runtime state");
}

void TestLuaExtensionSandbox() {
    px::test::TempDirectory temp("behavior");
    const auto extensionDirectory = temp.path / "Content" / "Extensions";
    std::filesystem::create_directories(extensionDirectory);
    {
        std::ofstream module(extensionDirectory / "helper.lua");
        module << "return { value = 42 }\n";
        std::ofstream entry(extensionDirectory / "demo.lua");
        entry << "local h=require('helper')\n"
                 "assert(h.value==42)\n"
                 "Engine.RegisterCommand('demo.command', function(args) return true end)\n"
                 "Engine.RegisterCommand('demo.await', function(args) Engine.AwaitSeconds(0.01); return true end)\n"
                 "Engine.RegisterAction('demo.typed', function(args, context)\n"
                 "  local amount = args.amount\n"
                 "  Engine.AwaitSeconds(0.01)\n"
                 "  return amount\n"
                 "end)\n";
        std::ofstream manifest(extensionDirectory / "default.pxextension");
        manifest
            << R"json({"format":"PrismatiXExtension","version":4,"id":"demo","entry":"demo.lua","capabilities":["runtime","ui"],"commands":[{"id":"demo.command","displayName":"Demo","description":"Typed command metadata test","category":"Story","await":false,"rollback":"transient","allowAdditionalParameters":false,"parameters":[{"name":"mode","displayName":"Mode","description":"Typed enum","type":"string","required":true,"default":"alpha","enum":["alpha","beta"],"editorHint":"enum"},{"name":"amount","displayName":"Amount","type":"number","default":1.5,"range":{"minimum":0.0,"maximum":10.0}},{"name":"asset","displayName":"Asset","type":"resource","default":"Content/Images/test.png","resourceFilter":"image","editorHint":"resource"},{"name":"tint","label":"Tint (legacy label)","type":"color","default":[255,128,64,255],"editorHint":"color"},{"name":"optionalNote","displayName":"Optional note","type":"string","default":null},{"name":"explicitNull","displayName":"Explicit null","type":"null","default":null}]},{"id":"demo.await","displayName":"Await","category":"Extension","await":true,"rollback":"boundary","parameters":[]}],"actions":[{"id":"demo.typed","displayName":"Typed Action","description":"Manifest metadata test","category":"Extension","reentry":"Restart","capabilities":["runtime","ui"],"parameters":[{"name":"mode","displayName":"Mode","description":"Typed enum","type":"string","required":true,"default":"alpha","enum":["alpha","beta"],"editorHint":"enum"},{"name":"amount","displayName":"Amount","type":"number","default":1.5,"range":{"minimum":0.0,"maximum":10.0}},{"name":"asset","displayName":"Asset","type":"resource","default":{"id":"11111111-1111-4111-8111-111111111111","path":"Assets/cg/rain.png","uuid":"11111111-1111-4111-8111-111111111111","kind":"cg","name":"Rain"},"resourceFilter":"image","editorHint":"resource"},{"name":"tint","displayName":"Tint","type":"color","default":[255,128,64,255],"editorHint":"color"},{"name":"optionalNote","displayName":"Optional note","type":"string","default":null}]}]})json";
        std::ofstream studioEntry(extensionDirectory / "studio-shape.lua");
        studioEntry
            << "Engine.RegisterCommand('demo.studioShape', function(args) "
               "assert(args.optionalNote == nil); return true end)\n"
               "Engine.RegisterCommand('demo.additional', function(args) "
               "return args.future == true end)\n";
        std::ofstream studioManifest(extensionDirectory /
                                     "studio-shape.pxextension");
        studioManifest << R"json({
  "format": "PrismatiXExtension",
  "version": 4,
  "id": "studio-shape",
  "entry": "studio-shape.lua",
  "capabilities": ["runtime"],
  "commands": [
    {
      "id": "demo.studioShape",
      "displayName": "Studio Shape",
      "category": "Tests",
      "allowAdditionalParameters": false,
      "parameters": [
        {"name":"optionalNote","displayName":"Optional note","type":"string","required":false,"default":null,"enum":[],"resourceFilter":"","editorHint":"default","range":null},
        {"name":"amount","displayName":"Amount","type":"number","required":false,"default":null,"enum":[],"resourceFilter":"","editorHint":"default","range":null},
        {"name":"asset","displayName":"Asset","type":"resource","required":false,"default":null,"enum":[],"resourceFilter":"image/*","editorHint":"resource","range":null}
      ]
    },
    {
      "id": "demo.additional",
      "displayName": "Additional",
      "category": "Tests",
      "allowAdditionalParameters": true,
      "parameters": []
    }
  ],
  "actions": []
})json";
        std::ofstream denied(extensionDirectory / "denied.pxextension");
        denied << R"({"format":"PrismatiXExtension","version":4,"id":"denied","entry":"demo.lua","capabilities":["filesystem"],"commands":[]})";
        std::ofstream missingEntry(extensionDirectory / "missing.lua");
        missingEntry << "return true\n";
        std::ofstream missing(extensionDirectory / "missing.pxextension");
        missing << R"({"format":"PrismatiXExtension","version":4,"id":"missing","entry":"missing.lua","capabilities":["runtime"],"commands":[{"id":"missing.command","parameters":[]}],"actions":[]})";
        std::ofstream collision(extensionDirectory / "collision.pxextension");
        collision << R"({"format":"PrismatiXExtension","version":4,"id":"collision","entry":"missing.lua","capabilities":["runtime"],"commands":[{"id":"say","parameters":[]}],"actions":[]})";
    }
    px::io::VFS vfs;
    vfs.MountDirectory(temp.path.string());
    px::lua::LuaServices services;
    services.vfs = &vfs;
    std::vector<px::lua::LuaConsoleMessage> console;
    services.console = [&console](const px::lua::LuaConsoleMessage& message) {
        console.push_back(message);
    };
    px::lua::LuaHost host(services);
    Check(host.RunString("print('console', 7)\nwarn('careful')",
                         "lua-console-output"),
          "Lua print and warn should remain valid base-library operations");
    Check(console.size() == 2 &&
              console[0].level == px::lua::LuaConsoleLevel::Info &&
              console[0].text == "console\t7" &&
              console[0].source == "lua-console-output" &&
              console[0].line == 1 &&
              console[1].level == px::lua::LuaConsoleLevel::Warning &&
              console[1].text == "careful" &&
              console[1].source == "lua-console-output" &&
              console[1].line == 2,
          "Lua console callback should preserve level, text, source, and line");
    Check(!host.RunString("error('console failure')", "lua-console-error") &&
              console.size() == 3 &&
              console.back().level == px::lua::LuaConsoleLevel::Error &&
              console.back().text.find("console failure") != std::string::npos &&
              console.back().source == "lua-console-error",
          "protected Lua failures should reach the dedicated error console sink");
    Check(host.RunString("assert(os == nil and io == nil and debug == nil and dofile == nil and loadfile == nil)"), "Lua should not expose host filesystem, process, or debug libraries by default");
    Check(host.LoadExtensionManifest("Content/Extensions/default.pxextension"), "declared extension should load through VFS-aware require");
    const auto* typedCommand = px::vn::CommandRegistry::Global().Find("demo.command");
    Check(
        typedCommand && typedCommand->description == "Typed command metadata test" &&
            typedCommand->category == "Story" &&
            typedCommand->rollbackPolicy == px::vn::RollbackPolicy::Transient &&
            !typedCommand->allowAdditionalParameters &&
            typedCommand->parameters.size() == 6 &&
            typedCommand->parameters[0].hasDefault &&
            typedCommand->parameters[0].defaultValue.TryGet<std::string>() &&
            *typedCommand->parameters[0].defaultValue.TryGet<std::string>() == "alpha" &&
            typedCommand->parameters[0].options.size() == 2 &&
            typedCommand->parameters[0].widget == px::vn::CommandEditorWidget::Enum &&
            typedCommand->parameters[1].minimum && typedCommand->parameters[1].maximum &&
            typedCommand->parameters[2].resourceType == "image" &&
            typedCommand->parameters[2].widget == px::vn::CommandEditorWidget::Resource &&
            typedCommand->parameters[3].label == "Tint (legacy label)" &&
            typedCommand->parameters[3].widget == px::vn::CommandEditorWidget::Color &&
            !typedCommand->parameters[4].hasDefault &&
            typedCommand->parameters[5].hasDefault &&
            typedCommand->parameters[5].defaultValue.Type() == px::VariantType::Null,
        "Lua command manifest should populate executable CommandRegistry metadata"
    );
    const auto* typedAction = px::ui::ActionCatalog::Global().Find("demo.typed");
    const auto* typedActionResource = typedAction && typedAction->arguments.size() > 2 &&
                                              typedAction->arguments[2].defaultValue
                                          ? typedAction->arguments[2].defaultValue->TryGet<px::ResourceRefValue>()
                                          : nullptr;
    Check(
        typedAction && typedAction->reentryPolicy == px::ui::ActionReentryPolicy::Restart && typedAction->capabilities.size() == 2 && typedAction->arguments.size() == 5 && typedAction->arguments[0].enumValues.size() == 2 &&
            typedAction->arguments[1].minimum && typedAction->arguments[1].maximum && typedAction->arguments[2].resourceType == "image" && typedActionResource && typedActionResource->id.ToString() == "11111111-1111-4111-8111-111111111111" && typedActionResource->lastKnownPath == "Assets/cg/rain.png" && typedAction->arguments[3].defaultValue && typedAction->arguments[3].defaultValue->TryGet<px::Color>() && !typedAction->arguments[4].defaultValue,
        "Lua Action manifest should preserve capabilities, defaults, enum, range, resource filter, editor hint, and reentry metadata"
    );
    Check(host.LoadExtensionManifest(
              "Content/Extensions/studio-shape.pxextension"),
          "Studio-serialized null default/range fields should load as absent optionals");
    const auto* studioShape =
        px::vn::CommandRegistry::Global().Find("demo.studioShape");
    Check(studioShape && studioShape->parameters.size() == 3 &&
              !studioShape->parameters[0].hasDefault &&
              !studioShape->parameters[0].minimum &&
              !studioShape->parameters[0].maximum &&
              !studioShape->parameters[1].hasDefault &&
              !studioShape->parameters[1].minimum &&
              !studioShape->parameters[1].maximum &&
              !studioShape->parameters[2].hasDefault &&
              studioShape->parameters[2].resourceType == "image/*" &&
              studioShape->parameters[2].widget ==
                  px::vn::CommandEditorWidget::Resource,
          "CommandRegistry should preserve the Studio manifest shape without inventing defaults or ranges");

    px::vn::Command studioInvocation;
    studioInvocation.type = "demo.studioShape";
    studioInvocation.typedArgs.emplace("amount", 2.0);
    studioInvocation.typedArgs.emplace(
        "asset",
        px::ResourceRefValue{px::Uuid::FromName("studio-shape.asset"),
                             "Assets/studio.png"});
    Check(px::vn::CommandRegistry::Global().Validate(studioInvocation),
          "typed command invocation should accept parameters that match the manifest");
    Check(host.InvokeCommand(studioInvocation),
          "Studio-shaped command should execute without treating null optionals as values");
    studioInvocation.typedArgs.emplace("future", true);
    Check(!px::vn::CommandRegistry::Global().Validate(studioInvocation),
          "allowAdditionalParameters=false should reject undeclared fields");
    px::vn::Command additionalInvocation;
    additionalInvocation.type = "demo.additional";
    additionalInvocation.typedArgs.emplace("future", true);
    Check(px::vn::CommandRegistry::Global().Validate(additionalInvocation),
          "allowAdditionalParameters=true should preserve undeclared typed fields");
    px::vn::Command invalidEnumInvocation;
    invalidEnumInvocation.type = "demo.command";
    invalidEnumInvocation.args = {{"mode", "removed"}};
    Check(!px::vn::CommandRegistry::Global().Validate(invalidEnumInvocation),
          "runtime command validation should enforce manifest enum choices");
    px::vn::Command invalidRangeInvocation;
    invalidRangeInvocation.type = "demo.command";
    invalidRangeInvocation.args = {{"mode", "alpha"}, {"amount", "11"}};
    Check(!px::vn::CommandRegistry::Global().Validate(invalidRangeInvocation),
          "runtime command validation should enforce manifest numeric ranges");
    px::vn::Command invalidResourceInvocation;
    invalidResourceInvocation.type = "demo.studioShape";
    invalidResourceInvocation.typedArgs.emplace("asset", true);
    Check(!px::vn::CommandRegistry::Global().Validate(
              invalidResourceInvocation),
          "runtime command validation should enforce manifest resource types");

    const auto invalidParameter = [](std::string id,
                                     px::vn::CommandParameterDescriptor parameter) {
        px::vn::CommandDescriptor descriptor;
        descriptor.id = std::move(id);
        descriptor.displayName = descriptor.id;
        descriptor.parameters.push_back(std::move(parameter));
        return px::vn::CommandRegistry::Global().Register(std::move(descriptor));
    };
    px::vn::CommandParameterDescriptor invalidEnum;
    invalidEnum.name = "mode";
    invalidEnum.label = "Mode";
    invalidEnum.type = px::VariantType::String;
    invalidEnum.options = {"same", "same"};
    Check(!invalidParameter("invalid.enum", std::move(invalidEnum)),
          "command schema should reject duplicate enum values");
    px::vn::CommandParameterDescriptor invalidRange;
    invalidRange.name = "amount";
    invalidRange.label = "Amount";
    invalidRange.type = px::VariantType::Number;
    invalidRange.minimum = 2.0;
    invalidRange.maximum = 1.0;
    Check(!invalidParameter("invalid.range", std::move(invalidRange)),
          "command schema should reject inverted numeric ranges");
    px::vn::CommandParameterDescriptor invalidDefault;
    invalidDefault.name = "mode";
    invalidDefault.label = "Mode";
    invalidDefault.type = px::VariantType::String;
    invalidDefault.options = {"alpha", "beta"};
    invalidDefault.defaultValue = std::string("removed");
    invalidDefault.hasDefault = true;
    Check(!invalidParameter("invalid.default", std::move(invalidDefault)),
          "command schema should reject defaults outside the enum");
    px::vn::CommandParameterDescriptor invalidResource;
    invalidResource.name = "asset";
    invalidResource.label = "Asset";
    invalidResource.type = px::VariantType::String;
    invalidResource.resourceType = "image/*";
    Check(!invalidParameter("invalid.resource", std::move(invalidResource)),
          "command schema should reject resource filters on non-resource parameters");
    px::vn::CommandParameterDescriptor invalidHint;
    invalidHint.name = "tint";
    invalidHint.label = "Tint";
    invalidHint.type = px::VariantType::String;
    invalidHint.widget = px::vn::CommandEditorWidget::Color;
    Check(!invalidParameter("invalid.hint", std::move(invalidHint)),
          "command schema should reject editor hints that disagree with parameter type");
    px::ui::ActionInvocation actionInvocation;
    actionInvocation.action = "demo.typed";
    actionInvocation.arguments = { { "mode", std::string("alpha") }, { "amount", 2.0 } };
    const auto actionStart = host.StartAction(actionInvocation);
    Check(actionStart.status && actionStart.pending, "Lua Action should run as a tracked coroutine");
    const auto actionCheckpoint = host.CapturePendingActions();
    Check(host.RestorePending({}) && host.RestorePendingActions(actionCheckpoint) && host.ActionState(actionStart.handle) == px::ui::ActionExecutionState::Running, "Lua Action should reconstruct its exact yield checkpoint");
    host.Update(0.02f);
    Check(host.ActionState(actionStart.handle) == px::ui::ActionExecutionState::Completed, "restored Lua Action should resume to completion");
    const auto cancelledAction = host.StartAction(actionInvocation);
    Check(cancelledAction.status && cancelledAction.pending,
          "Lua Action cancellation should begin from a tracked coroutine");
    host.CancelAction(cancelledAction.handle);
    host.Update(0.02f);
    Check(!host.HasPendingAction() &&
              host.ActionState(cancelledAction.handle) ==
                  px::ui::ActionExecutionState::Cancelled,
          "cancelled Lua Actions must not resume on a later frame");
    px::ui::ActionDispatcher luaActions;
    Check(luaActions.RegisterProvider(host.CreateActionProvider()),
          "Lua reentry dispatcher should register the production provider");
    const auto concurrentFirst = luaActions.Start(
        actionInvocation,
        {.reentryPolicy = px::ui::ActionReentryPolicy::Allow});
    const auto concurrentSecond = luaActions.Start(
        actionInvocation,
        {.reentryPolicy = px::ui::ActionReentryPolicy::Allow});
    Check(concurrentFirst && concurrentSecond &&
              concurrentFirst.Value() != concurrentSecond.Value() &&
              host.CapturePendingActions().size() == 2,
          "allow reentry should run independent Lua Action coroutines concurrently");
    host.Update(0.02f);
    luaActions.Update(0.0f);
    Check(luaActions.State(concurrentFirst.Value()) ==
              px::ui::ActionExecutionState::Completed &&
              luaActions.State(concurrentSecond.Value()) ==
                  px::ui::ActionExecutionState::Completed,
          "concurrent Lua Action coroutines should each resume to completion");
    luaActions.Forget(concurrentFirst.Value());
    luaActions.Forget(concurrentSecond.Value());
    const auto replacedAction = luaActions.Start(
        actionInvocation,
        {.reentryPolicy = px::ui::ActionReentryPolicy::Restart});
    const auto replacementAction = luaActions.Start(
        actionInvocation,
        {.reentryPolicy = px::ui::ActionReentryPolicy::Restart});
    Check(replacedAction && replacementAction &&
              luaActions.State(replacedAction.Value()) ==
                  px::ui::ActionExecutionState::Cancelled &&
              luaActions.State(replacementAction.Value()) ==
                  px::ui::ActionExecutionState::Running &&
              host.CapturePendingActions().size() == 1,
          "restart reentry should cancel only the prior Lua Action coroutine");
    host.Update(0.02f);
    luaActions.Update(0.0f);
    Check(luaActions.State(replacementAction.Value()) ==
              px::ui::ActionExecutionState::Completed,
          "replacement Lua Action should complete through the production provider");
    luaActions.Forget(replacedAction.Value());
    luaActions.Forget(replacementAction.Value());
    px::vn::Command awaitCommand;
    awaitCommand.type = "demo.await";
    Check(host.InvokeCommand(awaitCommand) && host.HasPendingCommand(), "Lua custom commands should suspend as coroutines instead of blocking the runtime");
    const auto luaCheckpoint = host.CapturePending();
    host.CancelPending();
    Check(host.RestorePending(luaCheckpoint) && host.HasPendingCommand(), "Lua commands should reconstruct the exact declared await boundary from a save");
    host.Update(0.02f);
    Check(!host.HasPendingCommand(), "Lua timer await should resume at a frame-safe checkpoint");
    const auto breakpoints = host.SetDebugBreakpoints(
        {{"Content/Extensions/demo.lua", 6}});
    Check(breakpoints.size() == 1, "Lua debugger should accept a source breakpoint");
    const auto debugStart = host.StartAction(actionInvocation);
    const auto& firstDebugState = host.CaptureDebugState();
    Check(debugStart.status && debugStart.pending && firstDebugState.paused &&
              firstDebugState.reason == "breakpoint" && !firstDebugState.frames.empty() &&
              firstDebugState.frames.front().line == 6,
          "Lua Action should pause at a verified source line and capture its frame");
    bool foundArgs = false;
    for (const auto& local : firstDebugState.frames.front().locals) {
        foundArgs = foundArgs || (local.name == "args" && local.value == "<table>");
    }
    Check(foundArgs, "Lua debugger should expose frame locals without opening the debug library");
    const auto watchedMode = host.EvaluateDebugWatch("args.mode");
    Check(watchedMode && watchedMode->value == "alpha",
          "Lua debugger should resolve side-effect-free local table watches");
    Check(!host.EvaluateDebugWatch("args[mode]"),
          "Lua debugger should reject executable watch syntax");
    Check(host.DebugStep(), "Lua debugger should step from a paused coroutine");
    host.Update(0.0f);
    const auto& steppedDebugState = host.CaptureDebugState();
    Check(steppedDebugState.paused && steppedDebugState.reason == "step" &&
              !steppedDebugState.frames.empty() &&
              steppedDebugState.frames.front().line == 7,
          "Lua debugger should pause on the following executable line");
    const auto watchedAmount = host.EvaluateDebugWatch("amount");
    Check(watchedAmount && watchedAmount->value.starts_with("2"),
          "stepping should refresh primitive local watches");
    Check(host.DebugContinue(), "Lua debugger should continue the paused coroutine");
    host.Update(0.0f);
    host.Update(0.02f);
    Check(host.ActionState(debugStart.handle) == px::ui::ActionExecutionState::Completed,
          "continued Lua Action should resume through its declared await point");
    Check(!host.LoadExtensionManifest("Content/Extensions/missing.pxextension") &&
              !px::vn::CommandRegistry::Global().Find("missing.command"),
          "declared commands without Engine.RegisterCommand callbacks must fail without poisoning the registry");
    Check(!host.LoadExtensionManifest("Content/Extensions/collision.pxextension") &&
              px::vn::CommandRegistry::Global().Find("say") != nullptr,
          "extension commands must not silently replace or reuse built-in descriptors");
    Check(!host.LoadExtensionManifest("Content/Extensions/denied.pxextension"), "undeployed filesystem capability should be rejected explicitly");
}


class CheckpointActionProvider final : public px::ui::IActionProvider {
public:
    explicit CheckpointActionProvider(
        std::string providerId = "checkpoint-test",
        std::string action = "test.behavior.async")
        : m_providerId(std::move(providerId)), m_action(std::move(action)) {}

    [[nodiscard]] std::string_view ProviderId() const override { return m_providerId; }
    [[nodiscard]] px::ui::ActionOrigin Origin() const override { return px::ui::ActionOrigin::BuiltIn; }
    [[nodiscard]] bool CanInvoke(std::string_view action) const override { return action == m_action; }
    px::Status Invoke(const px::ui::ActionInvocation&) override { return px::Status::Ok(); }
    px::ui::ProviderActionStart Start(const px::ui::ActionInvocation&) override {
        const auto id = m_next++;
        m_states[id] = px::ui::ActionExecutionState::Running;
        return { px::Status::Ok(), id, true };
    }
    [[nodiscard]] px::ui::ActionExecutionState Poll(const std::uint64_t handle) const override {
        const auto found = m_states.find(handle);
        return found == m_states.end() ? px::ui::ActionExecutionState::Unknown : found->second;
    }
    void Cancel(const std::uint64_t handle) override {
        ++m_cancelCount;
        m_states[handle] = px::ui::ActionExecutionState::Cancelled;
    }
    void Update(float) override {
        for (auto& [_, state] : m_states)
            if (state == px::ui::ActionExecutionState::Running) state = px::ui::ActionExecutionState::Completed;
    }
    [[nodiscard]] std::size_t StartCount() const { return m_next - 1; }
    [[nodiscard]] std::size_t CancelCount() const { return m_cancelCount; }

private:
    std::string m_providerId;
    std::string m_action;
    std::unordered_map<std::uint64_t, px::ui::ActionExecutionState> m_states;
    std::uint64_t m_next = 1;
    std::size_t m_cancelCount = 0;
};

void TestActionDispatcherReentryAndCancellation() {
    constexpr std::string_view actionId = "test.behavior.async";
    px::ui::ActionCatalog catalog;
    px::ui::ActionDescriptor descriptor;
    descriptor.id = actionId;
    descriptor.label = "Async";
    descriptor.displayName = "Async";
    descriptor.category = "Test";
    descriptor.origin = px::ui::ActionOrigin::BuiltIn;
    descriptor.providerId = "checkpoint-test";
    Check(catalog.Register(std::move(descriptor)),
          "reentry test Action should register");
    auto provider = std::make_shared<CheckpointActionProvider>();
    px::ui::ActionDispatcher actions(catalog);
    Check(actions.RegisterProvider(provider),
          "reentry test provider should register");
    const px::ui::ActionInvocation invocation{.action = std::string(actionId)};

    const auto allowFirst = actions.Start(
        invocation, {.reentryPolicy = px::ui::ActionReentryPolicy::Allow});
    const auto allowSecond = actions.Start(
        invocation, {.reentryPolicy = px::ui::ActionReentryPolicy::Allow});
    Check(allowFirst && allowSecond && allowFirst.Value() != allowSecond.Value() &&
              actions.CaptureState().size() == 2,
          "allow reentry should retain both concurrent executions");
    Check(actions.Cancel(allowFirst.Value()) && actions.Cancel(allowSecond.Value()) &&
              actions.State(allowFirst.Value()) ==
                  px::ui::ActionExecutionState::Cancelled &&
              actions.State(allowSecond.Value()) ==
                  px::ui::ActionExecutionState::Cancelled,
          "explicit cancellation should reach every provider execution");
    actions.Forget(allowFirst.Value());
    actions.Forget(allowSecond.Value());

    const auto ignored = actions.Start(
        invocation,
        {.reentryPolicy = px::ui::ActionReentryPolicy::IgnoreWhileRunning});
    Check(ignored && actions.Dispatch(
                         invocation,
                         {.reentryPolicy =
                              px::ui::ActionReentryPolicy::IgnoreWhileRunning}) &&
              provider->StartCount() == 3 && actions.CaptureState().size() == 1,
          "ignore-while-running should reuse one execution without starting another");
    actions.Update(0.1f);
    Check(actions.State(ignored.Value()) ==
              px::ui::ActionExecutionState::Completed,
          "an ignored fire-and-forget duplicate must not steal awaited execution ownership");
    actions.Forget(ignored.Value());

    const auto restartFirst = actions.Start(
        invocation, {.reentryPolicy = px::ui::ActionReentryPolicy::Restart});
    const auto restartSecond = actions.Start(
        invocation, {.reentryPolicy = px::ui::ActionReentryPolicy::Restart});
    Check(restartFirst && restartSecond &&
              restartFirst.Value() != restartSecond.Value() &&
              actions.State(restartFirst.Value()) ==
                  px::ui::ActionExecutionState::Cancelled &&
              actions.State(restartSecond.Value()) ==
                  px::ui::ActionExecutionState::Running &&
              provider->CancelCount() == 3,
          "restart should cancel the old execution while retaining its terminal state for awaiters");
    actions.Update(0.1f);
    Check(actions.State(restartSecond.Value()) ==
              px::ui::ActionExecutionState::Completed,
          "the replacement execution should continue to completion");
    actions.Forget(restartFirst.Value());
    actions.Forget(restartSecond.Value());

    const auto lifecycleFirst = actions.Start(
        invocation, {.reentryPolicy = px::ui::ActionReentryPolicy::Allow});
    const auto lifecycleSecond = actions.Start(
        invocation, {.reentryPolicy = px::ui::ActionReentryPolicy::Allow});
    const auto lifecycleCheckpoints = actions.CaptureState();
    actions.CancelAll();
    const bool providersCancelled = std::ranges::all_of(
        lifecycleCheckpoints, [&](const px::ui::ActionExecutionCheckpoint& checkpoint) {
            return provider->Poll(checkpoint.providerHandle) ==
                   px::ui::ActionExecutionState::Cancelled;
        });
    Check(lifecycleFirst && lifecycleSecond && providersCancelled &&
              actions.State(lifecycleFirst.Value()) ==
                  px::ui::ActionExecutionState::Unknown &&
              actions.State(lifecycleSecond.Value()) ==
                  px::ui::ActionExecutionState::Unknown,
          "lifecycle cancellation should terminate providers and release dispatcher handles");

    px::ui::ActionInvocation detachedInvocation = invocation;
    detachedInvocation.context.sourceScene = "UI/Detached.pxui";
    const auto detached = actions.Start(
        std::move(detachedInvocation),
        {.reentryPolicy = px::ui::ActionReentryPolicy::Allow});
    actions.Forget(detached.Value());
    const auto detachedCheckpoint = actions.CaptureState();
    actions.CancelSource("UI/Detached.pxui");
    Check(detached && detachedCheckpoint.size() == 1 &&
              detachedCheckpoint.front().autoForget &&
              provider->Poll(detachedCheckpoint.front().providerHandle) ==
                  px::ui::ActionExecutionState::Cancelled &&
              actions.State(detached.Value()) ==
                  px::ui::ActionExecutionState::Unknown,
          "detached Behavior Actions should stay tracked until completion or owning UI reset");

    constexpr std::string_view lifecycleActionId = "test.ui.lifecycle.async";
    px::ui::ActionDescriptor lifecycleDescriptor;
    lifecycleDescriptor.id = lifecycleActionId;
    lifecycleDescriptor.label = "UI lifecycle async";
    lifecycleDescriptor.displayName = "UI lifecycle async";
    lifecycleDescriptor.category = "Test";
    lifecycleDescriptor.origin = px::ui::ActionOrigin::BuiltIn;
    lifecycleDescriptor.providerId = "checkpoint-ui-lifecycle";
    Check(px::ui::ActionCatalog::Global().Register(std::move(lifecycleDescriptor)),
          "UI lifecycle Action should register");
    auto lifecycleProvider = std::make_shared<CheckpointActionProvider>(
        "checkpoint-ui-lifecycle", std::string(lifecycleActionId));
    px::ui::UIContext context;
    Check(context.Actions().RegisterProvider(lifecycleProvider) &&
              context.SetRoot(std::make_unique<px::ui::Control>("FirstRoot")) &&
              context.ConfigureTriggers({}, std::nullopt, "UI/Lifecycle.pxui"),
          "UI lifecycle context should install its provider and initial root");
    px::ui::ActionInvocation lifecycleInvocation;
    lifecycleInvocation.action = lifecycleActionId;
    lifecycleInvocation.context.sourceScene = "UI/Lifecycle.pxui";
    const auto uiExecution = context.Actions().Start(std::move(lifecycleInvocation));
    const auto uiCheckpoint = context.Actions().CaptureState();
    Check(uiExecution && uiCheckpoint.size() == 1 &&
              context.SetRoot(std::make_unique<px::ui::Control>("ReplacementRoot")) &&
              lifecycleProvider->Poll(uiCheckpoint.front().providerHandle) ==
                  px::ui::ActionExecutionState::Cancelled &&
              context.Actions().State(uiExecution.Value()) ==
                  px::ui::ActionExecutionState::Unknown,
          "replacing the shared UI root should cancel in-flight trigger Actions");
}

void TestBehaviorGraphExecutionAndCheckpoint() {
    Check(px::ui::RegisterBuiltinUITypes(),
          "Behavior integration must explicitly register runtime UI metadata");
    const auto entry = px::Uuid::FromName("behavior.test.entry"), action = px::Uuid::FromName("behavior.test.action"), constant = px::Uuid::FromName("behavior.test.constant"), set = px::Uuid::FromName("behavior.test.set");
    px::ui::BehaviorGraph graph;
    graph.nodes = { { .id = entry, .kind = px::ui::BehaviorNodeKind::SignalEntry },
                    { .id = action, .kind = px::ui::BehaviorNodeKind::Action, .properties = { { "action", std::string("test.behavior.async") }, { "wait", true } } },
                    { .id = constant, .kind = px::ui::BehaviorNodeKind::Constant, .properties = { { "value", std::int64_t{ 7 } } } },
                    { .id = set, .kind = px::ui::BehaviorNodeKind::SetVariable, .properties = { { "name", std::string("result") } } } };
    graph.links = { { px::Uuid::FromName("behavior.link.1"), entry, "out", action, "in" }, { px::Uuid::FromName("behavior.link.2"), action, "out", set, "in" }, { px::Uuid::FromName("behavior.link.3"), constant, "value", set, "value" } };
    Check(graph.Validate("behavior-test"), "valid typed Behavior Graph should pass validation");
    px::ui::ActionCatalog catalog;
    px::ui::ActionDescriptor descriptor;
    descriptor.id = "test.behavior.async";
    descriptor.label = "Async";
    descriptor.displayName = "Async";
    descriptor.category = "Test";
    descriptor.origin = px::ui::ActionOrigin::BuiltIn;
    descriptor.providerId = "checkpoint-test";
    Check(catalog.Register(std::move(descriptor)), "Behavior test Action should register");
    auto provider = std::make_shared<CheckpointActionProvider>();
    px::ui::ActionDispatcher firstActions(catalog);
    Check(firstActions.RegisterProvider(provider), "Behavior test provider should register");
    std::int64_t result = 0;
    px::ui::BehaviorRuntimeServices services;
    services.actions = &firstActions;
    services.writeVariable = [&](std::string_view, const px::Variant& value) {
        const auto* integer = value.TryGet<std::int64_t>();
        if (integer) result = *integer;
        return px::Status::Ok();
    };
    px::ui::BehaviorGraphRunner first(services);
    Check(first.SetGraph(graph, "behavior-test"), "Behavior runner should accept valid graph");
    const auto started = first.Start(entry);
    Check(started && first.ActiveFibers().size() == 1 && first.ActiveFibers().front().actionExecution != 0, "awaited Action should suspend a Behavior fiber");
    const auto actionCheckpoint = firstActions.CaptureState();
    const auto fiberCheckpoint = first.CaptureState();
    px::ui::ActionDispatcher restoredActions(catalog);
    Check(restoredActions.RegisterProvider(provider), "restored dispatcher should register provider");
    Check(restoredActions.RestoreState(actionCheckpoint), "Action execution handles should restore before Behavior fibers");
    services.actions = &restoredActions;
    px::ui::BehaviorGraphRunner restored(services);
    Check(restored.SetGraph(graph, "behavior-test"), "restored runner should load the same graph");
    Check(restored.RestoreState(fiberCheckpoint), "Behavior fiber checkpoint should restore");
    restoredActions.Update(0.1f);
    restored.Update(0.1f);
    Check(result == 7 && restored.ActiveFibers().empty(), "restored async Action should resume the fiber and preserve typed data flow");

    px::ui::BehaviorGraph typedError;
    const auto branch = px::Uuid::FromName("behavior.bad.branch"), text = px::Uuid::FromName("behavior.bad.text");
    typedError.nodes = { { .id = branch, .kind = px::ui::BehaviorNodeKind::Branch }, { .id = text, .kind = px::ui::BehaviorNodeKind::Constant, .properties = { { "value", std::string("not bool") } } } };
    typedError.links = { { px::Uuid::Random(), text, "value", branch, "condition" } };
    Check(!typedError.Validate(), "Behavior Graph should reject incompatible typed pins");
    px::ui::BehaviorGraph cycle;
    const auto a = px::Uuid::FromName("behavior.cycle.a"), b = px::Uuid::FromName("behavior.cycle.b");
    cycle.nodes = { { .id = a, .kind = px::ui::BehaviorNodeKind::Sequence }, { .id = b, .kind = px::ui::BehaviorNodeKind::Sequence } };
    cycle.links = { { px::Uuid::Random(), a, "0", b, "in" }, { px::Uuid::Random(), b, "0", a, "in" } };
    Check(!cycle.Validate(), "Behavior Graph should reject flow cycles at build time");

    px::ui::UIContext context;
    Check(context.SetRoot(std::make_unique<px::ui::Control>("AnimationRoot")), "UI context should accept an animation test root");
    bool externalRunning = true;
    std::string externalPath;
    context.SetExternalAnimationServices(
        [&](const std::string_view path) {
            externalPath = path;
            return px::Result<std::uint64_t>::Success(42);
        },
        [&](const std::uint64_t handle) { return handle == 42 && externalRunning; }
    );
    const auto animationEntry = px::Uuid::Random(), playAnimation = px::Uuid::Random();
    px::ui::BehaviorGraph animationGraph;
    animationGraph.nodes = { { .id = animationEntry, .kind = px::ui::BehaviorNodeKind::SignalEntry },
                             { .id = playAnimation, .kind = px::ui::BehaviorNodeKind::PlayAnimation, .properties = { { "name", std::string("Content/Animations/HUD.pxanim") }, { "wait", true } } } };
    animationGraph.links = { { px::Uuid::Random(), animationEntry, "out", playAnimation, "in" } };
    Check(context.ConfigureTriggers({}, animationGraph, "animation-behavior"), "UI context should configure external animation Behavior nodes");
    const auto externalStarted = context.Behaviors().Start(animationEntry);
    Check(externalStarted && externalPath == "Content/Animations/HUD.pxanim" && context.Behaviors().ActiveFibers().size() == 1, "Play Animation must route external .pxanim paths through the shared runtime timeline");
    context.Behaviors().Update(.1f);
    Check(context.Behaviors().ActiveFibers().size() == 1, "waiting Behavior fibers must remain suspended while an external animation is running");
    externalRunning = false;
    context.Behaviors().Update(.1f);
    Check(context.Behaviors().ActiveFibers().empty(), "waiting Behavior fibers must resume when external animation playback completes");
    const auto localEntry = px::Uuid::Random(), localPlay = px::Uuid::Random(), localClipId = px::Uuid::Random(), localState = px::Uuid::Random();
    px::ui::AnimationClip localClip;
    localClip.id = localClipId;
    localClip.name = "Default";
    localClip.duration = .1f;
    localClip.tracks.push_back({ .node = context.Root()->Id(), .property = "opacity", .keys = { { 0.0f, 1.0, px::ui::Ease::Linear, px::ui::KeyInterpolation::Linear }, { .1f, 0.0, px::ui::Ease::Linear, px::ui::KeyInterpolation::Linear } } });
    px::ui::UIAnimationLibrary localLibrary;
    localLibrary.clips.push_back(std::move(localClip));
    localLibrary.machine.entry = localState;
    localLibrary.machine.states.push_back({ localState, "Default", localClipId, { 0, 0 } });
    Check(context.SetAnimations(std::move(localLibrary), false), "UI context should install the same local Animation Controller used by Player and Preview");
    px::ui::BehaviorGraph localGraph;
    localGraph.nodes = { { .id = localEntry, .kind = px::ui::BehaviorNodeKind::SignalEntry }, { .id = localPlay, .kind = px::ui::BehaviorNodeKind::PlayAnimation, .properties = { { "name", std::string("Default") }, { "wait", true } } } };
    localGraph.links = { { px::Uuid::Random(), localEntry, "out", localPlay, "in" } };
    Check(context.ConfigureTriggers({}, localGraph, "local-animation-behavior") && context.Behaviors().Start(localEntry), "Behavior Flow should play a local State through the shared Animation Controller");
    px::Input emptyInput;
    (void)context.Update(emptyInput, 100, 100, .05f);
    Check(context.Behaviors().ActiveFibers().size() == 1, "waiting Behavior Flow must remain suspended while a local Clip is active");
    (void)context.Update(emptyInput, 100, 100, .06f);
    Check(context.Behaviors().ActiveFibers().empty(), "waiting Behavior Flow must resume when the local Clip completes");
}


}  // namespace

int main() {
    Run("UIRouter_RouteStackAndModalState", TestRouteStackAndModalState);
    Run("Lua_ExtensionSandbox", TestLuaExtensionSandbox);
    Run("ActionDispatcher_ReentryAndCancellation",
        TestActionDispatcherReentryAndCancellation);
    Run("Behavior_ExecutionAndCheckpoint", TestBehaviorGraphExecutionAndCheckpoint);
    if (g_failures == 0) std::cout << "PASS: behavior integration\n";
    return g_failures == 0 ? 0 : 1;
}
