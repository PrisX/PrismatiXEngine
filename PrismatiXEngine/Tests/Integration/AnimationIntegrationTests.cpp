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


void TestUnifiedTimelineAndPresets() {
    px::animation::AnimationClip clip;
    clip.id = px::Uuid::FromName("timeline-test");
    clip.name = "Test";
    clip.duration = 1.0f;
    clip.tracks.push_back({ { px::animation::TargetKind::Camera, "main", "zoom" }, { { 0.0f, 1.0, px::animation::Curve::Linear }, { 1.0f, 2.0, px::animation::Curve::EaseInOut } } });
    clip.markers.push_back({ 0.5f, "half", {} });
    px::animation::TimelinePlayer player;
    double applied = 0.0;
    int markers = 0;
    int completed = 0;
    player.SetApply([&](const px::animation::TrackBinding&, const px::Variant& value) {
        if (const auto* number = value.TryGet<double>()) applied = *number;
        return px::Status::Ok();
    });
    player.SetEvent([&](const px::animation::Marker&) { ++markers; });
    player.SetCompletion([&](px::animation::PlaybackHandle, bool cancelled) {
        if (!cancelled) ++completed;
    });
    Check(static_cast<bool>(player.Register(clip)), "valid unified animation clip should register");
    const auto handle = player.Play(clip.id, true);
    player.Update(0.6f);
    Check(handle != 0 && applied > 1.0 && applied < 2.0 && markers == 1, "timeline should sample curves and emit markers while playing");
    const auto state = player.CaptureState();
    player.Update(0.5f);
    Check(!player.Playing(handle) && completed == 1, "awaitable timeline playback should complete without blocking the thread");
    Check(player.RestoreState(state) && player.Playing(handle), "timeline playback should restore its exact mid-animation state");
    const std::string encoded = px::animation::WriteAnimationClip(clip);
    auto decoded = px::animation::ParseAnimationClip(encoded, "memory.pxanim");
    Check(decoded && decoded.Value().id == clip.id && decoded.Value().tracks.size() == 1, ".pxanim version 4 should round-trip typed keyframes exactly");
    px::animation::AnimationClip child;
    child.id = px::Uuid::FromName("timeline-child");
    child.name = "Child";
    child.duration = 1.0f;
    child.tracks.push_back({ { px::animation::TargetKind::UI, "panel", "alpha" }, { { 0.0f, 0.0, px::animation::Curve::Linear }, { 1.0f, 1.0, px::animation::Curve::Linear } } });
    px::animation::AnimationClip parent;
    parent.id = px::Uuid::FromName("timeline-parent");
    parent.name = "Parent";
    parent.duration = 2.0f;
    parent.nested.push_back({ 0.5f, child.id, 2.0f });
    Check(player.Register(child) && player.Register(parent), "nested clips should register as ordinary reusable animation assets");
    const auto nestedHandle = player.Play(parent.id);
    player.Update(0.75f);
    Check(nestedHandle != 0 && applied > 0.4 && applied < 0.6, "nested clips should be sampled with start offset and speed");
    Check(px::animation::OfficialPresets().size() >= 35, "official preset library should cover text, actor, screen, and UI effects");
}


void TestEmbeddedUIAnimationParity() {
    Check(px::ui::RegisterBuiltinUITypes(), "built-in UI types should register for animation playback");
    px::resource::TypedDocument scene;
    scene.kind = px::resource::DocumentKind::Scene;
    scene.id = px::Uuid::Random();
    scene.type = "UIScene";
    scene.properties["uiSchemaVersion"] = std::int64_t{ 5 };
    px::resource::NodeRecord root;
    root.id = px::Uuid::Random();
    root.type = "Control";
    root.name = "Animated";
    scene.nodes.push_back(root);
    px::ui::AnimationClip clip;
    clip.id = px::Uuid::Random();
    clip.name = "Default";
    clip.duration = 1.0f;
    clip.tracks.push_back({ .node = root.id, .property = "opacity", .keys = { { 0.0f, 1.0, px::ui::Ease::Linear, px::ui::KeyInterpolation::Linear }, { 1.0f, 0.0, px::ui::Ease::Linear, px::ui::KeyInterpolation::Linear } } });
    px::ui::UIAnimationLibrary library;
    const px::Uuid clipId = clip.id, stateId = px::Uuid::Random();
    library.clips.push_back(std::move(clip));
    library.machine.entry = stateId;
    library.machine.states.push_back({ stateId, "Default", clipId, { 80, 80 } });
    scene.properties["animations"] = px::ui::WriteUIAnimationLibrary(library);
    const auto loaded = px::ui::InstantiateUIScene(scene, nullptr, px::ui::FormatterRegistry{});
    Check(loaded && loaded.Value().animations.has_value(), "strict UI scene loader must parse the v5 Animation Library used by Preview and Player");
    if (loaded && loaded.Value().animations) {
        auto& control = *loaded.Value().root;
        px::ui::AnimationPlayer player(control);
        const auto* loadedClip = loaded.Value().animations->FindClip(clipId);
        Check(loadedClip && player.Play(*loadedClip), "embedded UI Clip should start");
        Check(player.Seek(.5f), "UI Clip should scrub to an arbitrary time");
        Check(std::abs(control.Opacity() - .5f) < .001f, "Editor scrub and runtime playback must use the same typed property interpolation");
        Check(player.Stop(true), "stopping UI animation should restore the authored value");
        Check(std::abs(control.Opacity() - 1.0f) < .001f, "animation preview must restore the design state");
    }
    scene.properties["animations"] = px::VariantObject{};
    Check(!px::ui::InstantiateUIScene(scene, nullptr, px::ui::FormatterRegistry{}), "malformed v5 Animation Libraries must fail strict scene loading");
}

void TestUIAnimationStateMachine() {
    auto root = std::make_unique<px::ui::Control>("AnimationMachine");
    const px::Uuid target = root->Id();
    px::ui::UIAnimationController controller(*root);
    const px::Uuid idleClipId = px::Uuid::Random(), hoverClipId = px::Uuid::Random(), pressedClipId = px::Uuid::Random();
    const auto makeClip = [&](const px::Uuid& id, std::string name, double opacity) {
        px::ui::AnimationClip clip;
        clip.id = id;
        clip.name = std::move(name);
        clip.duration = 1.0f;
        clip.tracks.push_back({ .node = target, .property = "opacity", .keys = { { 0.0f, opacity, px::ui::Ease::Linear, px::ui::KeyInterpolation::Linear }, { 1.0f, opacity, px::ui::Ease::Linear, px::ui::KeyInterpolation::Linear } } });
        return clip;
    };
    px::ui::UIAnimationLibrary library;
    library.clips.push_back(makeClip(idleClipId, "Idle", 1.0));
    library.clips.push_back(makeClip(hoverClipId, "Hover", 0.0));
    library.clips.push_back(makeClip(pressedClipId, "Pressed", .25));
    const px::Uuid idle = px::Uuid::Random(), hover = px::Uuid::Random(), pressed = px::Uuid::Random();
    library.machine.entry = idle;
    library.machine.states = { { idle, "Idle", idleClipId, { 0, 0 } }, { hover, "Hover", hoverClipId, { 200, 0 } }, { pressed, "Pressed", pressedClipId, { 400, 0 } } };
    library.machine.parameters = { { "go", px::ui::AnimationParameterType::Trigger, false }, { "enabled", px::ui::AnimationParameterType::Bool, false }, { "speed", px::ui::AnimationParameterType::Number, 0.0 } };
    const px::Uuid toHover = px::Uuid::Random(), fallback = px::Uuid::Random(), anyPressed = px::Uuid::Random(), exitPressed = px::Uuid::Random();
    library.machine.transitions = { { toHover, idle, hover, { { "go", px::ui::AnimationConditionOperator::Triggered, false }, { "speed", px::ui::AnimationConditionOperator::GreaterEqual, 2.0 } }, false, 1, .2f, 10 },
                                    { fallback, idle, pressed, { { "go", px::ui::AnimationConditionOperator::Triggered, false }, { "speed", px::ui::AnimationConditionOperator::GreaterEqual, 2.0 } }, false, 1, 0, 1 },
                                    { anyPressed, std::nullopt, pressed, { { "enabled", px::ui::AnimationConditionOperator::Equal, true } }, false, 1, 0, -100 },
                                    { exitPressed, pressed, idle, {}, true, .5f, 0, 0 } };
    const px::Variant encoded = px::ui::WriteUIAnimationLibrary(library);
    auto decoded = px::ui::ParseUIAnimationLibrary(encoded, "animation-machine-test");
    Check(static_cast<bool>(decoded), "Animation Library must save and load all State Machine data");
    if (!decoded) return;
    px::Variant duplicateClip = encoded.Clone();
    auto* duplicateClips = (*duplicateClip.AsObject())["clips"].AsArray();
    (*duplicateClips->at(1).AsObject())["id"] = idleClipId;
    Check(!px::ui::ParseUIAnimationLibrary(duplicateClip, "duplicate-clip-test"), "Animation Library must reject duplicate Clip UUIDs");
    px::Variant missingClip = encoded.Clone();
    auto* missingStates = (*(*missingClip.AsObject())["machine"].AsObject())["states"].AsArray();
    (*missingStates->at(0).AsObject())["clip"] = px::Uuid::Random();
    Check(!px::ui::ParseUIAnimationLibrary(missingClip, "missing-clip-test"), "Animation Library must reject State references to missing Clips");
    px::Variant missingParameter = encoded.Clone();
    auto* missingTransitions = (*(*missingParameter.AsObject())["machine"].AsObject())["transitions"].AsArray();
    auto* conditions = (*missingTransitions->at(0).AsObject())["conditions"].AsArray();
    (*conditions->at(0).AsObject())["parameter"] = std::string("missing");
    Check(!px::ui::ParseUIAnimationLibrary(missingParameter, "missing-parameter-test"), "Animation Library must reject Transition references to missing Parameters");
    Check(controller.SetLibrary(decoded.TakeValue()), "Animation Controller must enter the required Entry state");
    Check(controller.SetTrigger("go"), "Trigger parameter should be set");
    Check(controller.Update(.01f), "unsatisfied Number condition should not transition");
    auto state = controller.CaptureState();
    Check(state.state == idle && state.parameters.at("go").TryGet<bool>() && *state.parameters.at("go").TryGet<bool>(), "Trigger must remain armed until a transition succeeds");
    Check(controller.SetNumber("speed", 2.0) && controller.Update(0), "Number >= condition should select the high-priority transition");
    state = controller.CaptureState();
    Check(state.state == hover && state.transition == toHover && state.parameters.at("go").TryGet<bool>() && !*state.parameters.at("go").TryGet<bool>(), "successful transition must consume Trigger and preserve transition identity");
    Check(controller.Update(.1f) && std::abs(root->Opacity() - .5f) < .06f, "Number properties must cross-fade over Transition Duration");
    Check(controller.Pause(), "Animation Controller should pause the shared player");
    const auto blendCheckpoint = controller.CaptureState();
    Check(blendCheckpoint.paused && controller.Resume() && controller.Travel(pressed) && controller.RestoreState(blendCheckpoint), "paused active transition checkpoints should restore");
    state = controller.CaptureState();
    Check(controller.Paused() && state.transition == toHover && std::abs(state.transitionProgress - .5f) < .01f && std::abs(state.position - .1f) < .01f, "pause state, transition progress, and Clip position must survive capture/restore");
    Check(controller.Resume(), "restored Animation Controller should resume");
    Check(controller.Travel(idle) && controller.SetTrigger("go") && controller.SetBool("enabled", true) && controller.Update(0), "Any State transition should run");
    state = controller.CaptureState();
    Check(state.state == pressed && state.transition == anyPressed && state.parameters.at("go").TryGet<bool>() && *state.parameters.at("go").TryGet<bool>(), "Any State must win before current-state Priority and consume only its own Trigger conditions");
    Check(controller.SetBool("enabled", false) && controller.Update(.4f), "Exit Time update should run");
    Check(controller.CaptureState().state == pressed, "transition must wait until normalized Exit Time");
    Check(controller.Update(.2f) && controller.CaptureState().state == idle, "transition must fire after Exit Time");
    const auto checkpoint = controller.CaptureState();
    Check(controller.Travel("Hover") && controller.RestoreState(checkpoint) && controller.CaptureState().state == idle, "Animation Controller state and parameters must restore deterministically");
}

}  // namespace

int main() {
    Run("Timeline_PresetsAndCheckpoint", TestUnifiedTimelineAndPresets);
    Run("Animation_EmbeddedPreviewRuntimeParity", TestEmbeddedUIAnimationParity);
    Run("Animation_StateMachineCheckpoint", TestUIAnimationStateMachine);
    if (g_failures == 0) std::cout << "PASS: animation integration\n";
    return g_failures == 0 ? 0 : 1;
}
