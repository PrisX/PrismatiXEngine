#include <algorithm>
#include <array>
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
    Check(!archive.Contains("Content\\test.txt") &&
              !archive.Read("Content\\test.txt").has_value(),
          "archive reads must reject backslash aliases instead of normalizing them");

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
    px::io::ArchiveWriter backslash;
    backslash.Add("Content\\escape.txt", expected);
    Check(!backslash.Write((temp.path / "backslash.pdx").string()),
          "archive writer must reject non-canonical backslash paths");

    const auto maliciousPath = temp.path / "malicious-index.pdx";
    px::io::ArchiveWriter maliciousWriter;
    maliciousWriter.SetCompression(false);
    maliciousWriter.Add("Content/a.txt", expected);
    Check(maliciousWriter.Write(maliciousPath.string()),
          "malicious-index fixture should begin as a valid archive");
    {
        std::fstream stream(maliciousPath,
                            std::ios::binary | std::ios::in | std::ios::out);
        std::array<unsigned char, 8> encodedOffset{};
        stream.seekg(12, std::ios::beg);
        stream.read(reinterpret_cast<char*>(encodedOffset.data()),
                    static_cast<std::streamsize>(encodedOffset.size()));
        std::uint64_t indexOffset = 0;
        for (std::size_t byte = 0; byte < encodedOffset.size(); ++byte)
            indexOffset |= static_cast<std::uint64_t>(encodedOffset[byte]) <<
                           (byte * 8);
        // index count (4), hash (8), name length (2), then the 13-byte name.
        stream.seekp(static_cast<std::streamoff>(indexOffset + 14),
                     std::ios::beg);
        constexpr char traversal[] = "../escape.txt";
        static_assert(sizeof(traversal) - 1 == 13);
        stream.write(traversal, sizeof(traversal) - 1);
    }
    px::io::Archive malicious;
    Check(!malicious.Open(maliciousPath.string()),
          "archive reader must reject traversal entries even when the index is hostile");

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
    snapshot.gameId = "save-integration-game";
    snapshot.packageFingerprint = std::string(64, 'a');
    snapshot.contentVersion = "fixture-v1";
    snapshot.saveVersion = 1;
    snapshot.anchor = {"story-document", "choice-source", "choice-operation"};
    snapshot.scriptPath = "Content/Scenario/start.pxscenario";
    snapshot.pc = 7;
    snapshot.chapter = "Chapter 1";
    snapshot.variables["affection"] = 3;
    snapshot.typedVariables["route"] = px::vn::Value("alice");
    snapshot.typedVariables["flags"] = px::vn::Value(px::vn::ValueMap{ { "ending", px::vn::Value(true) }, { "scores", px::vn::Value(px::vn::ValueList{ 1, 2, 3 }) } });
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
    snapshot.stage.ruleActive = true;
    snapshot.stage.ruleOldBackground = "Content/Background/hall.png";
    snapshot.stage.ruleNewBackground = "Content/Background/room.png";
    snapshot.stage.ruleMask = "Content/Transitions/clouds.png";
    snapshot.stage.ruleProgress = 0.35f;
    snapshot.stage.ruleDuration = 1.2f;
    snapshot.stage.ruleVague = 48;
    snapshot.stage.cameraX = 16.0f;
    snapshot.stage.cameraY = -9.0f;
    snapshot.stage.cameraZoom = 1.15f;
    snapshot.stage.screenEffects["vignette"] = 0.3f;
    snapshot.stage.actors.push_back({ "alice", "Content/Characters/alice.png", 2, 4.0f, -2.0f, 1.0f, {}, 180.0f, 255.0f, 0.0f, 640.0f, 640.0f, false });
    snapshot.stage.layers.push_back({
        .name = "foreground",
        .imagePath = "Content/Images/card.png",
        .x = 18.0f,
        .y = 30.0f,
        .scale = 1.25f,
        .alpha = 220,
        .z = 3,
        .scaleY = 0.75f,
        .rotation = 17.0f});
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
    px::script::PendingCommandState pendingEffect;
    pendingEffect.command.type = "demo.awaitEffect";
    pendingEffect.yieldIndex = 1;
    pendingEffect.waitKind = "screen-effect";
    pendingEffect.handle = 91;
    snapshot.scriptPending.push_back(std::move(pendingEffect));
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
    snapshot.ui.behavior.fibers.push_back(std::move(behaviorFiber));
    px::ui::ActionInvocation savedActionInvocation;
    savedActionInvocation.action = "demo.async";
    savedActionInvocation.arguments["message"] = px::Variant(std::string("checkpoint"));
    savedActionInvocation.context.sourceScene = "Content/UI/HUD.pxscene";
    savedActionInvocation.context.sourceNode = behaviorDelay;
    savedActionInvocation.context.signal = "behavior";
    snapshot.ui.behavior.actions.push_back({ 41, savedActionInvocation, "script", 73, false });
    px::ui::UIAnimationRuntimeState uiAnimation;
    uiAnimation.state = px::Uuid::FromName("save.ui.animation.state");
    uiAnimation.transition = px::Uuid::FromName("save.ui.animation.transition");
    uiAnimation.position = 0.42f;
    uiAnimation.transitionProgress = 0.65f;
    uiAnimation.paused = true;
    uiAnimation.parameters["selected"] = px::Variant(true);
    snapshot.ui.animation = std::move(uiAnimation);
    px::ui::VisualStateGroupRuntimeState visualGroup;
    visualGroup.group = "interaction";
    visualGroup.state = "hover";
    visualGroup.from = "normal";
    visualGroup.elapsed = 0.08f;
    visualGroup.duration = 0.12f;
    visualGroup.easing = px::ui::VisualStateEase::EaseOut;
    visualGroup.transitionFrom.push_back(
        {behaviorDelay, "opacity", px::Variant(0.4)});
    visualGroup.animationClip = px::Uuid::FromName("save.ui.visual.clip");
    visualGroup.animationPosition = 0.08f;
    snapshot.ui.visualState = px::ui::VisualStateRuntimeState{
        {std::move(visualGroup)}};
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
        loaded && loaded->vm.state == px::vn::VMState::WaitingChoice && loaded->vm.callStack.size() == 1 && loaded->vm.choices.size() == 1 && loaded->dialogue.state.displayText == "Hel" &&
            loaded->dialogue.speedMs == 42 && loaded->typedVariables.at("route").TryGet<std::string>() && loaded->typedVariables.at("flags").AsObject() && loaded->routes.stack.size() == 2 && loaded->routes.modals.size() == 1 &&
            loaded->timelines.size() == 1 && loaded->timelines.front().awaiting && loaded->animationClips.size() == 1 && loaded->animationClips.front().name == "Custom/Save" && loaded->stage.backgroundFade == 0.45f && loaded->stage.ruleActive && loaded->stage.ruleMask == "Content/Transitions/clouds.png" && loaded->stage.ruleProgress == 0.35f && loaded->stage.ruleVague == 48 && loaded->stage.cameraX == 16.0f && loaded->stage.cameraY == -9.0f && loaded->stage.cameraZoom == 1.15f && loaded->stage.actors.size() == 1 && loaded->stage.layers.size() == 1 && loaded->stage.layers.front().scaleY == 0.75f && loaded->stage.layers.front().rotation == 17.0f &&
            loaded->stage.tweens.size() == 1 && loaded->audio.music.playbackFrame == 24000 && loaded->scriptPending.size() == 2 && loaded->scriptPending.front().yieldIndex == 1 && loaded->scriptPending.back().waitKind == "screen-effect" && loaded->scriptPending.back().handle == 91 && loaded->scriptActions.size() == 1 && loaded->scriptActions.front().yieldIndex == 2 &&
            loaded->ui.behavior.fibers.size() == 1 && loaded->ui.behavior.fibers.front().current == behaviorDelay && loaded->ui.behavior.fibers.front().signalArguments.at("position").TryGet<px::Vec2>() && loaded->ui.behavior.actions.size() == 1 &&
            loaded->ui.behavior.actions.front().providerHandle == 73 &&
            loaded->ui.animation && loaded->ui.animation->position == 0.42f &&
            loaded->ui.animation->parameters.at("selected").TryGet<bool>() &&
            loaded->ui.visualState && loaded->ui.visualState->groups.size() == 1 &&
            loaded->ui.visualState->groups.front().state == "hover" &&
            loaded->ui.visualState->groups.front().animationPosition == 0.08f,
        "current save schema should preserve exact VM, stage, audio, script, UI, and variable state"
    );

    const auto currentJson = px::progress::LoadJson(saves.SlotPath(0), nullptr);
    Check(currentJson && currentJson->value("schemaRevision", 0) == 4 &&
              currentJson->contains("integrityHash") &&
              currentJson->contains("anchor") && currentJson->contains("state") &&
              (*currentJson)["state"].contains("scriptPending") &&
              (*currentJson)["state"].contains("scriptActions") &&
              (*currentJson)["state"].contains("ui"),
          "current saves must use an authenticated v2 envelope and complete runtime state");
    if (currentJson) {
        auto tampered = *currentJson;
        tampered["state"]["chapter"] = "tampered";
        Check(px::progress::SaveJson(saves.SlotPath(4), tampered, nullptr) &&
                  !saves.Load(4),
              "save envelope must reject payload changes that do not update integrity metadata");
    }

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
    profile.SetVariable("affection", px::Variant(std::int64_t{7}));
    profile.UnlockScene("scene.after-story");
    profile.UnlockCG("cg.sunset");
    profile.UnlockMusic("music.ending");
    Check(px::progress::SaveGlobalProfile(profile, profilePath, nullptr), "global profile should be written with the current schema");

    px::progress::GlobalProfile loadedProfile;
    Check(px::progress::LoadGlobalProfile(loadedProfile, profilePath, nullptr), "global profile should load with the current schema");
    Check(
        loadedProfile.HasSeen("scene.intro#line-1") && loadedProfile.HasChoiceSeen("choice.intro#option-a") && loadedProfile.ClearCount() == 1 && loadedProfile.RouteCleared("route.alice") &&
            loadedProfile.Variable("affection") && loadedProfile.Variable("affection")->TryGet<std::int64_t>() && *loadedProfile.Variable("affection")->TryGet<std::int64_t>() == 7 && loadedProfile.SceneUnlocked("scene.after-story") && loadedProfile.CGUnlocked("cg.sunset") && loadedProfile.MusicUnlocked("music.ending"),
        "global profile should preserve seen state, clears, persistent variables, and unlocks"
    );

    px::progress::Json legacyProfile{ { "version", 4 }, { "clearCount", 99 } };
    Check(px::progress::SaveJson(profilePath, legacyProfile, nullptr), "legacy profile fixture should be written");
    Check(!px::progress::LoadGlobalProfile(loadedProfile, profilePath, nullptr), "legacy numeric profile versions must be rejected");
    px::progress::Json revisionOneProfile{
        {"format", "PrismatiXProfile"}, {"schemaRevision", 1},
        {"seen", px::progress::Json::array()},
        {"choicesSeen", px::progress::Json::array()},
        {"clearedRoutes", px::progress::Json::array()},
        {"scenes", px::progress::Json::array()},
        {"cgs", px::progress::Json::array()},
        {"music", px::progress::Json::array()},
        {"vars", px::progress::Json::object()}, {"clearCount", 0}};
    Check(!loadedProfile.ApplyJson(revisionOneProfile),
          "revision-1 profiles with unstable seen identities must be rejected");

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
    px::progress::Json revisionOneSettings{
        {"format", "PrismatiXSettings"}, {"schemaRevision", 1}};
    Check(px::progress::SaveJson(settingsPath, revisionOneSettings, nullptr),
          "revision-1 settings fixture should be written");
    Check(!loadedSettings.Load(settingsPath, nullptr),
          "revision-1 settings must be rejected by the 0.2 runtime");
}

void TestExplicitSaveMigrationChain() {
    px::progress::SaveSnapshot source;
    source.gameId = "commercial-vn";
    source.packageFingerprint = std::string(64, 'a');
    source.contentVersion = "chapter-1";
    source.saveVersion = 1;
    source.anchor = {"document-old", "line-old", "operation-old"};
    source.scriptPath = "Content/Runtime/old.pxir";
    source.vm.scriptPath = source.scriptPath;
    source.typedVariables["affection"] = px::vn::Value(std::int64_t{7});
    source.variables["affection"] = 7;
    source.routes.stack = {"old-hud"};

    const std::vector<px::progress::SaveMigrationDescriptor> migrations{
        {"chapter-1-to-1-5", "chapter-1", 1, "chapter-1.5", 1,
         "Content/Migrations/chapter-1.pxsave-migration"},
        {"chapter-1-5-to-2", "chapter-1.5", 1, "chapter-2", 2,
         "Content/Migrations/chapter-2.pxsave-migration"}};
    const auto readMigration = [](const std::string_view asset)
        -> std::optional<std::string> {
        if (asset.ends_with("chapter-1.pxsave-migration")) {
            return R"({
              "format":"PrismatiXSaveMigration","schemaRevision":2,
              "id":"chapter-1-to-1-5",
              "from":{"contentVersion":"chapter-1","saveVersion":1},
              "to":{"contentVersion":"chapter-1.5","saveVersion":1},
              "anchor":{"policy":"preserve"},
              "operations":[
                {"op":"renameVariable","from":"affection","to":"rinAffinity"},
                {"op":"setVariable","name":"routeUnlocked","value":true},
                {"op":"renameRoute","from":"old-hud","to":"hud"}
              ]
            })";
        }
        if (asset.ends_with("chapter-2.pxsave-migration")) {
            return R"({
              "format":"PrismatiXSaveMigration","schemaRevision":2,
              "id":"chapter-1-5-to-2",
              "from":{"contentVersion":"chapter-1.5","saveVersion":1},
              "to":{"contentVersion":"chapter-2","saveVersion":2},
              "anchor":{"policy":"map","mappings":[{
                "from":{"runtimeDocumentId":"document-old","sourceId":"line-old","operationId":"operation-old"},
                "to":{"runtimeDocumentId":"document-new","sourceId":"line-new","operationId":"operation-new"}
              }]},
              "operations":[{"op":"setScript","path":"Content/Runtime/new.pxir"}]
            })";
        }
        return std::nullopt;
    };
    auto migrated = px::progress::MigrateSaveSnapshot(
        source,
        {"commercial-vn", std::string(64, 'b'), "chapter-2", 2},
        migrations, readMigration);
    Check(static_cast<bool>(migrated),
          "an explicit, deterministic multi-step migration should succeed");
    if (migrated) {
        const auto& value = migrated.Value();
        Check(value.contentVersion == "chapter-2" && value.saveVersion == 2 &&
                  value.packageFingerprint == std::string(64, 'b') &&
                  value.anchor.runtimeDocumentId == "document-new" &&
                  value.anchor.sourceId == "line-new" &&
                  value.anchor.operationId == "operation-new" &&
                  value.scriptPath == "Content/Runtime/new.pxir" &&
                  value.vm.scriptPath == "Content/Runtime/new.pxir" &&
                  value.typedVariables.contains("rinAffinity") &&
                  !value.typedVariables.contains("affection") &&
                  value.typedVariables.at("routeUnlocked").TryGet<bool>() &&
                  *value.typedVariables.at("routeUnlocked").TryGet<bool>() &&
                  value.routes.stack == std::vector<std::string>{"hud"},
              "migration transforms state and stable anchor before adopting the new package identity");
    }
    Check(source.contentVersion == "chapter-1" &&
              source.anchor.operationId == "operation-old" &&
              source.typedVariables.contains("affection") &&
              !source.typedVariables.contains("rinAffinity"),
          "successful migration operates on an isolated copy");

    auto missingChain = px::progress::MigrateSaveSnapshot(
        source,
        {"commercial-vn", std::string(64, 'b'), "chapter-2", 2}, {},
        readMigration);
    Check(!missingChain && source.anchor.operationId == "operation-old",
          "missing migration rejects without mutating the current save candidate");

    auto missingAnchor = migrations;
    missingAnchor.erase(missingAnchor.begin());
    missingAnchor.front().fromContentVersion = "chapter-1";
    auto badAnchor = px::progress::MigrateSaveSnapshot(
        source,
        {"commercial-vn", std::string(64, 'b'), "chapter-2", 2},
        missingAnchor,
        [](std::string_view) -> std::optional<std::string> {
            return R"({
              "format":"PrismatiXSaveMigration","schemaRevision":2,
              "id":"chapter-1-5-to-2",
              "from":{"contentVersion":"chapter-1","saveVersion":1},
              "to":{"contentVersion":"chapter-2","saveVersion":2},
              "anchor":{"policy":"map","mappings":[{
                "from":{"runtimeDocumentId":"other","sourceId":"other","operationId":"other"},
                "to":{"runtimeDocumentId":"new","sourceId":"new","operationId":"new"}
              }]},"operations":[]
            })";
        });
    Check(!badAnchor && source.anchor.operationId == "operation-old",
          "a migration without an exact anchor mapping is transactionally rejected");
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
    Run("Save_ExplicitMigrationChainIsTransactional", TestExplicitSaveMigrationChain);
    Run("Assets_SavedIdentityRegisters", TestSavedAssetIdentityRegistration);
    if (g_failures == 0) std::cout << "PASS: runtime-save-load integration\n";
    return g_failures == 0 ? 0 : 1;
}

