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
#include "Engine/Platform/Input.h"
#include "Engine/Progression/GameSettings.h"
#include "Engine/Progression/GlobalProfile.h"
#include "Engine/Progression/GlobalProfileStore.h"
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


void TestArchiveBoundsAndRoundTrip() {
    px::test::TempDirectory temp("runtime-save-load");
    const auto archivePath = temp.path / "valid.pdx";
    px::io::ArchiveWriter writer;
    writer.SetCompression(true);
    const px::io::Bytes expected{ 'P', 'r', 'i', 's', 'm', 'a' };
    writer.Add("Content/test.txt", expected);
    Check(writer.Write(archivePath.string()), "valid archive should be written");

    px::io::Archive archive;
    Check(archive.Open(archivePath.string()), "valid archive should open");
    Check(archive.Contains("Content/test.txt"), "archive should contain exact normalized path");
    const auto actual = archive.Read("Content/test.txt");
    Check(actual && *actual == expected, "archive payload should round-trip");
    Check(!archive.Contains("Content/other.txt"), "archive should reject missing path");

    const auto corruptPath = temp.path / "corrupt.pdx";
    {
        std::ofstream stream(corruptPath, std::ios::binary);
        stream.write("PDX4", 4);
    }
    px::io::Archive corrupt;
    Check(!corrupt.Open(corruptPath.string()), "truncated archive must be rejected");

    px::io::ArchiveWriter unsafe;
    unsafe.Add("../escape.txt", expected);
    Check(!unsafe.Write((temp.path / "unsafe.pdx").string()), "archive writer must reject traversal paths");
    const auto encryptedPath = temp.path / "encrypted.pdx";
    px::io::ArchiveWriter encrypted;
    const auto key = px::crypto::DeriveKey("test-key");
    encrypted.SetKey(key);
    encrypted.Add("Content/secure.txt", expected);
    Check(encrypted.Write(encryptedPath.string()), "authenticated archive should build");
    px::io::Archive secured;
    Check(secured.Open(encryptedPath.string(), &key) && secured.Read("Content/secure.txt") == expected, "AES-GCM archive should decrypt and authenticate");
    {
        std::fstream stream(encryptedPath, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekg(-1, std::ios::end);
        char byte = 0;
        stream.read(&byte, 1);
        byte ^= 0x40;
        stream.seekp(-1, std::ios::end);
        stream.write(&byte, 1);
    }
    px::io::Archive tampered;
    Check(!tampered.Open(encryptedPath.string(), &key), "authenticated archive must reject a modified index");
}

void TestSaveValidation() {
    px::test::TempDirectory temp("runtime-save-load");
    px::progress::SaveSystem saves;
    saves.Configure(temp.path.string(), nullptr);

    px::progress::SaveSnapshot snapshot;
    snapshot.scriptPath = "Content/Scenario/start.pxscenario";
    snapshot.pc = 7;
    snapshot.chapter = "Chapter 1";
    snapshot.variables["affection"] = 3;
    snapshot.typedVariables["route"] = px::vn::Value("alice");
    snapshot.typedVariables["flags"] = px::vn::Value(px::vn::ValueMap{ { "ending", px::vn::Value(true) }, { "scores", px::vn::Value(px::vn::ValueList{ 1, 2, 3 }) } });
    snapshot.persistentVariables.insert("affection");
    snapshot.vm.scriptPath = snapshot.scriptPath;
    snapshot.vm.pc = 8;
    snapshot.vm.state = px::vn::VMState::WaitingChoice;
    snapshot.vm.callStack.push_back({ "Content/Scenario/prologue.pxscenario", 12 });
    snapshot.vm.choices.push_back({ "Stay", "stay" });
    snapshot.vm.speaker = "Alice";
    snapshot.vm.textEffect = "pulse";
    snapshot.vm.timerRemainingMs = 250;
    snapshot.dialogue.state.speaker = "Alice";
    snapshot.dialogue.state.fullText = "Hello";
    snapshot.dialogue.state.displayText = "Hel";
    snapshot.dialogue.state.currentChar = 3;
    snapshot.dialogue.state.totalChars = 5;
    snapshot.dialogue.state.effect = "pulse";
    snapshot.dialogue.state.effectProgress = 0.5f;
    snapshot.dialogue.speedMs = 42;
    snapshot.routes.stack = { "game", "settings" };
    snapshot.routes.modals = { "confirm" };
    snapshot.timelines.push_back({ 17, px::Uuid::FromName("PrismatiX.OfficialPreset.Text.fade"), 0.2f, 1.0f, 0, true, true });
    px::animation::AnimationClip savedClip;
    savedClip.id = px::Uuid::FromName("save-custom-clip");
    savedClip.name = "Custom/Save";
    savedClip.duration = 1.0f;
    savedClip.tracks.push_back({ { px::animation::TargetKind::Stage, "alice", "alpha" }, { { 0.0f, 0.0, px::animation::Curve::Linear }, { 1.0f, 1.0, px::animation::Curve::EaseOut } } });
    snapshot.animationClips.push_back(std::move(savedClip));
    snapshot.stage.background = "Content/Background/room.png";
    snapshot.stage.previousBackground = "Content/Background/hall.png";
    snapshot.stage.backgroundFade = 0.45f;
    snapshot.stage.cameraZoom = 1.15f;
    snapshot.stage.screenEffects["vignette"] = 0.3f;
    snapshot.stage.actors.push_back({ "alice", "Content/Characters/alice.png", 2, 4.0f, -2.0f, 1.0f, {}, 180.0f, 255.0f, 0.0f, 640.0f, 640.0f, false });
    px::vn::Stage::SavedTween tween;
    tween.target = "alice";
    tween.spec.hasX = true;
    tween.spec.x = 24.0f;
    tween.elapsed = 0.2f;
    tween.duration = 0.6f;
    snapshot.stage.tweens.push_back(tween);
    snapshot.audio.music = { "Content/Audio/theme.ogg", true, true, 24000 };
    px::script::PendingCommandState pending;
    pending.command.type = "demo.await";
    pending.command.args.push_back({ "duration", "0.5" });
    pending.yieldIndex = 1;
    pending.waitKind = "timer";
    pending.remainingSeconds = 0.25f;
    snapshot.scriptPending.push_back(std::move(pending));
    const px::Uuid behaviorEntry = px::Uuid::FromName("save.behavior.entry");
    const px::Uuid behaviorDelay = px::Uuid::FromName("save.behavior.delay");
    px::ui::BehaviorFiberState behaviorFiber;
    behaviorFiber.id = 9;
    behaviorFiber.entry = behaviorEntry;
    behaviorFiber.current = behaviorDelay;
    behaviorFiber.continuation.push_back(behaviorEntry);
    behaviorFiber.delayRemaining = 0.35f;
    behaviorFiber.actionExecution = 41;
    behaviorFiber.signalArguments["position"] = px::Variant(px::Vec2{ 12.0f, 24.0f });
    snapshot.behavior.fibers.push_back(std::move(behaviorFiber));
    px::ui::ActionInvocation savedActionInvocation;
    savedActionInvocation.action = "demo.async";
    savedActionInvocation.arguments["message"] = px::Variant(std::string("checkpoint"));
    savedActionInvocation.context.sourceScene = "Content/UI/HUD.pxscene";
    savedActionInvocation.context.sourceNode = behaviorDelay;
    savedActionInvocation.context.signal = "behavior";
    snapshot.behavior.actions.push_back({ 41, savedActionInvocation, "script", 73, false });
    px::script::PendingActionState pendingAction;
    pendingAction.id = 73;
    pendingAction.invocation = savedActionInvocation;
    pendingAction.yieldIndex = 2;
    pendingAction.waitKind = "timer";
    pendingAction.remainingSeconds = 0.35f;
    snapshot.scriptActions.push_back(std::move(pendingAction));
    Check(saves.Save(0, snapshot), "valid save should be written");
    const auto loaded = saves.Load(0);
    Check(loaded && loaded->pc == 7 && loaded->variables.at("affection") == 3, "valid save should round-trip");
    Check(
        loaded && loaded->persistentVariables.contains("affection") && loaded->vm.state == px::vn::VMState::WaitingChoice && loaded->vm.callStack.size() == 1 && loaded->vm.choices.size() == 1 && loaded->dialogue.state.displayText == "Hel" &&
            loaded->dialogue.speedMs == 42 && loaded->typedVariables.at("route").TryGet<std::string>() && loaded->typedVariables.at("flags").AsObject() && loaded->routes.stack.size() == 2 && loaded->routes.modals.size() == 1 &&
            loaded->timelines.size() == 1 && loaded->timelines.front().awaiting && loaded->animationClips.size() == 1 && loaded->animationClips.front().name == "Custom/Save" && loaded->stage.backgroundFade == 0.45f && loaded->stage.actors.size() == 1 &&
            loaded->stage.tweens.size() == 1 && loaded->audio.music.playbackFrame == 24000 && loaded->scriptPending.size() == 1 && loaded->scriptPending.front().yieldIndex == 1 && loaded->scriptActions.size() == 1 && loaded->scriptActions.front().yieldIndex == 2 &&
            loaded->behavior.fibers.size() == 1 && loaded->behavior.fibers.front().current == behaviorDelay && loaded->behavior.fibers.front().signalArguments.at("position").TryGet<px::Vec2>() && loaded->behavior.actions.size() == 1 &&
            loaded->behavior.actions.front().providerHandle == 73,
        "current save schema should preserve exact VM, stage, audio, script Action, Behavior, and variable state"
    );

    const auto currentJson = px::progress::LoadJson(saves.SlotPath(0), nullptr);
    Check(currentJson && currentJson->value("schemaRevision", 0) == 2 &&
              currentJson->contains("scriptPending") &&
              currentJson->contains("scriptActions"),
          "current saves must use language-neutral script checkpoint fields");

    px::progress::Json wrongType{ { "format", "PrismatiXSave" }, { "schemaRevision", 2 }, { "variables", { { "affection", "high" } } } };
    Check(px::progress::SaveJson(saves.SlotPath(1), wrongType, nullptr), "wrong-type fixture should be written");
    Check(!saves.Load(1), "wrong-type save must fail without throwing");

    px::progress::Json legacySave{ { "version", 4 }, { "scriptPath", "removed" } };
    Check(px::progress::SaveJson(saves.SlotPath(3), legacySave, nullptr), "legacy fixture should be written");
    Check(!saves.Load(3), "legacy numeric save versions must be rejected");

    px::progress::Json wrongSchema{ { "format", "PrismatiXSave" }, { "schemaRevision", 999 } };
    Check(px::progress::SaveJson(saves.SlotPath(2), wrongSchema, nullptr), "wrong-schema fixture should be written");
    Check(!saves.Load(2), "unsupported save schema must be rejected");
    Check(!saves.Peek(2).exists, "unsupported save must not appear as a valid slot");
}

void TestProfileAndSettingsSchemas() {
    px::test::TempDirectory temp("runtime-profile-settings");
    const auto profilePath = (temp.path / "profile.pxprofile").string();
    const auto settingsPath = (temp.path / "settings.pxsettings").string();

    px::progress::GlobalProfile profile;
    profile.MarkSeen("scene.intro#line-1");
    profile.MarkChoiceSeen("choice.intro#option-a");
    profile.RegisterClear("route.alice");
    profile.SetPersistentVar("affection", 7);
    profile.UnlockScene("scene.after-story");
    profile.UnlockCG("cg.sunset");
    profile.UnlockMusic("music.ending");
    Check(px::progress::SaveGlobalProfile(profile, profilePath, nullptr), "global profile should be written with the current schema");

    px::progress::GlobalProfile loadedProfile;
    Check(px::progress::LoadGlobalProfile(loadedProfile, profilePath, nullptr), "global profile should load with the current schema");
    Check(
        loadedProfile.HasSeen("scene.intro#line-1") && loadedProfile.HasChoiceSeen("choice.intro#option-a") && loadedProfile.ClearCount() == 1 && loadedProfile.RouteCleared("route.alice") &&
            loadedProfile.PersistentVar("affection") == 7 && loadedProfile.SceneUnlocked("scene.after-story") && loadedProfile.CGUnlocked("cg.sunset") && loadedProfile.MusicUnlocked("music.ending"),
        "global profile should preserve seen state, clears, persistent variables, and unlocks"
    );

    px::progress::Json legacyProfile{ { "version", 4 }, { "clearCount", 99 } };
    Check(px::progress::SaveJson(profilePath, legacyProfile, nullptr), "legacy profile fixture should be written");
    Check(!px::progress::LoadGlobalProfile(loadedProfile, profilePath, nullptr), "legacy numeric profile versions must be rejected");

    px::progress::GameSettings settings;
    settings.bgmVolume = 64;
    settings.voiceVolume = 96;
    settings.language = "ja-JP";
    settings.textScale = 1.25f;
    settings.highContrast = true;
    settings.reducedMotion = true;
    settings.selfVoicing = true;
    Check(settings.Save(settingsPath, nullptr), "game settings should be written with the current schema");

    px::progress::GameSettings loadedSettings;
    Check(loadedSettings.Load(settingsPath, nullptr), "game settings should load with the current schema");
    Check(
        loadedSettings.bgmVolume == 64 && loadedSettings.voiceVolume == 96 && loadedSettings.language == "ja-JP" && loadedSettings.textScale == 1.25f && loadedSettings.highContrast && loadedSettings.reducedMotion && loadedSettings.selfVoicing,
        "game settings should preserve audio, locale, text scale, and accessibility options"
    );

    px::progress::Json legacySettings{ { "version", 4 }, { "language", "legacy" } };
    Check(px::progress::SaveJson(settingsPath, legacySettings, nullptr), "legacy settings fixture should be written");
    Check(!loadedSettings.Load(settingsPath, nullptr), "legacy numeric settings versions must be rejected");
}


void TestSavedAssetIdentityRegistration() {
    px::test::TempDirectory temp("runtime-save-load");
    std::filesystem::create_directories(temp.path / "Content/UI");
    std::filesystem::create_directories(temp.path / "Content/Scenario");
    const auto scene = temp.path / "Content/UI/test.pxscene";
    const auto scenario = temp.path / "Content/Scenario/start.pxscenario";
    const auto layout = temp.path / "Content/Scenario/start.pxlayout";
    std::ofstream(scene) << "scene";
    std::ofstream(scenario) << "scenario";
    std::ofstream(layout) << "layout";
    px::resource::AssetRegistry registry;
    const auto registered = registry.RegisterAsset(temp.path, scene, "scene");
    const auto registeredScenario = registry.RegisterAsset(temp.path, scenario, "script");
    const auto registeredLayout = registry.RegisterAsset(temp.path, layout, "other");
    Check(static_cast<bool>(registered), "saved scene should receive identity metadata");
    Check(static_cast<bool>(registeredScenario) && static_cast<bool>(registeredLayout), "saved Scenario and its layout companion should both receive identity metadata");
    Check(std::filesystem::exists(px::resource::AssetRegistry::MetaPath(scene)), "scene registration should create a .pxmeta file");
    Check(std::filesystem::exists(px::resource::AssetRegistry::MetaPath(scenario)) && std::filesystem::exists(px::resource::AssetRegistry::MetaPath(layout)), "Scenario save should create identity metadata for both output files");
    Check(registry.Scan(temp.path), "asset scan should accept all registered saved artifacts");
}


}  // namespace

int main() {
    Run("Archive_BoundsAndRoundTrip", TestArchiveBoundsAndRoundTrip);
    Run("Save_RejectsInvalidAndRoundTrips", TestSaveValidation);
    Run("ProfileAndSettings_CurrentSchemasRoundTrip", TestProfileAndSettingsSchemas);
    Run("Assets_SavedIdentityRegisters", TestSavedAssetIdentityRegistration);
    if (g_failures == 0) std::cout << "PASS: runtime-save-load integration\n";
    return g_failures == 0 ? 0 : 1;
}

