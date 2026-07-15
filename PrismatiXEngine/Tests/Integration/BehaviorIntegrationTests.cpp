#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

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
        entry << "local h=require('helper'); assert(h.value==42); "
                 "Engine.RegisterCommand('demo.command', function(args) return true end); "
                 "Engine.RegisterCommand('demo.await', function(args) Engine.AwaitSeconds(0.01); return true end); "
                 "Engine.RegisterAction('demo.typed', function(args, context) assert(args.mode == 'alpha' and args.amount == 2.0); Engine.AwaitSeconds(0.01); return true end)\n";
        std::ofstream manifest(extensionDirectory / "default.pxextension");
        manifest
            << R"({"format":"PrismatiXExtension","version":4,"id":"demo","entry":"demo.lua","capabilities":["runtime","ui"],"commands":[{"id":"demo.command","displayName":"Demo","category":"Extension","await":false,"rollback":"reversible","parameters":[]},{"id":"demo.await","displayName":"Await","category":"Extension","await":true,"rollback":"boundary","parameters":[]}],"actions":[{"id":"demo.typed","displayName":"Typed Action","description":"Manifest metadata test","category":"Extension","reentry":"Restart","capabilities":["runtime","ui"],"parameters":[{"name":"mode","displayName":"Mode","description":"Typed enum","type":"string","required":true,"default":"alpha","enum":["alpha","beta"],"editorHint":"enum"},{"name":"amount","displayName":"Amount","type":"number","default":1.5,"range":{"minimum":0.0,"maximum":10.0}},{"name":"asset","displayName":"Asset","type":"resource","default":"Content/Images/test.png","resourceFilter":"image","editorHint":"resource"},{"name":"tint","displayName":"Tint","type":"color","default":[255,128,64,255],"editorHint":"color"}]}]})";
        std::ofstream denied(extensionDirectory / "denied.pxextension");
        denied << R"({"format":"PrismatiXExtension","version":4,"id":"denied","entry":"demo.lua","capabilities":["filesystem"],"commands":[]})";
    }
    px::io::VFS vfs;
    vfs.MountDirectory(temp.path.string());
    px::lua::LuaServices services;
    services.vfs = &vfs;
    px::lua::LuaHost host(services);
    Check(host.RunString("assert(os == nil and io == nil and debug == nil and dofile == nil and loadfile == nil)"), "Lua should not expose host filesystem, process, or debug libraries by default");
    Check(host.LoadExtensionManifest("Content/Extensions/default.pxextension"), "declared extension should load through VFS-aware require");
    const auto* typedAction = px::ui::ActionCatalog::Global().Find("demo.typed");
    Check(
        typedAction && typedAction->reentryPolicy == px::ui::ActionReentryPolicy::Restart && typedAction->capabilities.size() == 2 && typedAction->arguments.size() == 4 && typedAction->arguments[0].enumValues.size() == 2 &&
            typedAction->arguments[1].minimum && typedAction->arguments[1].maximum && typedAction->arguments[2].resourceType == "image" && typedAction->arguments[3].defaultValue && typedAction->arguments[3].defaultValue->TryGet<px::Color>(),
        "Lua Action manifest should preserve capabilities, defaults, enum, range, resource filter, editor hint, and reentry metadata"
    );
    px::ui::ActionInvocation actionInvocation;
    actionInvocation.action = "demo.typed";
    actionInvocation.arguments = { { "mode", std::string("alpha") }, { "amount", 2.0 } };
    const auto actionStart = host.StartAction(actionInvocation);
    Check(actionStart.status && actionStart.pending, "Lua Action should run as a tracked coroutine");
    const auto actionCheckpoint = host.CapturePendingActions();
    Check(host.RestorePending({}) && host.RestorePendingActions(actionCheckpoint) && host.ActionState(actionStart.handle) == px::ui::ActionExecutionState::Running, "Lua Action should reconstruct its exact yield checkpoint");
    host.Update(0.02f);
    Check(host.ActionState(actionStart.handle) == px::ui::ActionExecutionState::Completed, "restored Lua Action should resume to completion");
    px::vn::Command awaitCommand;
    awaitCommand.type = "demo.await";
    Check(host.InvokeCommand(awaitCommand) && host.HasPendingCommand(), "Lua custom commands should suspend as coroutines instead of blocking the runtime");
    const auto luaCheckpoint = host.CapturePending();
    host.CancelPending();
    Check(host.RestorePending(luaCheckpoint) && host.HasPendingCommand(), "Lua commands should reconstruct the exact declared await boundary from a save");
    host.Update(0.02f);
    Check(!host.HasPendingCommand(), "Lua timer await should resume at a frame-safe checkpoint");
    Check(!host.LoadExtensionManifest("Content/Extensions/denied.pxextension"), "undeployed filesystem capability should be rejected explicitly");
}


class CheckpointActionProvider final : public px::ui::IActionProvider {
public:
    [[nodiscard]] std::string_view ProviderId() const override { return "checkpoint-test"; }
    [[nodiscard]] px::ui::ActionOrigin Origin() const override { return px::ui::ActionOrigin::BuiltIn; }
    [[nodiscard]] bool CanInvoke(std::string_view action) const override { return action == "test.behavior.async"; }
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
    void Cancel(const std::uint64_t handle) override { m_states[handle] = px::ui::ActionExecutionState::Cancelled; }
    void Update(float) override {
        for (auto& [_, state] : m_states)
            if (state == px::ui::ActionExecutionState::Running) state = px::ui::ActionExecutionState::Completed;
    }

private:
    std::unordered_map<std::uint64_t, px::ui::ActionExecutionState> m_states;
    std::uint64_t m_next = 1;
};

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
    Run("Behavior_ExecutionAndCheckpoint", TestBehaviorGraphExecutionAndCheckpoint);
    if (g_failures == 0) std::cout << "PASS: behavior integration\n";
    return g_failures == 0 ? 0 : 1;
}
