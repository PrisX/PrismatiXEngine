#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <SDL3/SDL_stdinc.h>

#include "Engine/Animation/Timeline.h"
#include "Engine/Core/TypeRegistry.h"
#include "Engine/IO/Archive.h"
#include "Engine/IO/VFS.h"
#include "Engine/Platform/Input.h"
#include "Engine/Progression/GameSettings.h"
#include "Engine/Progression/GlobalProfile.h"
#include "Engine/Progression/GlobalProfileStore.h"
#include "Engine/Progression/AwaitedWork.h"
#include "Engine/Progression/Persist.h"
#include "Engine/Progression/SaveSystem.h"
#include "Engine/Progression/UserDataLocation.h"
#include "Engine/Resources/AssetRegistry.h"
#include "Engine/Support/Utf8Path.h"
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
    auto compressedStream = archive.OpenStream("Content/test.txt");
    std::array<std::uint8_t, 3> streamed{};
    Check(compressedStream && compressedStream->Size() == expected.size() &&
              compressedStream->Read(streamed.data(), streamed.size()) == 3 &&
              compressedStream->Seek(-2, px::io::SeekOrigin::End) &&
              compressedStream->Tell() == expected.size() - 2,
          "compressed archive entries should expose the seekable VFS contract");

    const auto streamingPath = temp.path / "streaming.pdx";
    px::io::ArchiveWriter streamingWriter;
    streamingWriter.SetCompression(false);
    streamingWriter.Add("Content/video.bin", expected);
    Check(streamingWriter.Write(streamingPath.string()),
          "uncompressed streaming archive should be written");
    px::io::VFS streamingVfs;
    Check(streamingVfs.MountArchive(streamingPath.string()),
          "uncompressed streaming archive should mount");
    auto directStream = streamingVfs.Open("Content/video.bin");
    std::array<std::uint8_t, 2> tail{};
    Check(directStream && directStream->Seek(-2, px::io::SeekOrigin::End) &&
              directStream->Read(tail.data(), tail.size()) == 2 &&
              tail[0] == 'm' && tail[1] == 'a',
          "uncompressed archive entries should seek and read without whole-entry allocation");
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
        .rotation = 17.0f,
        .action = px::vn::ActorActionRequest{"shake-head", 10.0f, 100, -1, ""},
        .actionElapsed = 1.5});
    snapshot.stage.backgroundAction = px::vn::ActorActionRequest{"float", 0.0f, 0, -1, "linear"};
    snapshot.stage.backgroundActionElapsed = 2.25;
    px::vn::Stage::SavedNode sceneRoot;
    sceneRoot.name = "scene-root";
    sceneRoot.kind = px::vn::Stage::NodeKind::Group;
    sceneRoot.children = {"alice", "foreground"};
    sceneRoot.transform = {.x = 2.0f, .y = 3.0f, .scaleX = 0.9f,
                           .scaleY = 1.1f, .rotation = 4.0f,
                           .opacity = 0.8f};
    sceneRoot.order = 1;
    snapshot.stage.nodes.push_back(std::move(sceneRoot));

    px::vn::Stage::SavedNode aliceNode;
    aliceNode.name = "alice";
    aliceNode.kind = px::vn::Stage::NodeKind::Character;
    aliceNode.parent = "scene-root";
    aliceNode.z = 2;
    aliceNode.order = 2;
    snapshot.stage.nodes.push_back(std::move(aliceNode));

    px::vn::Stage::SavedNode foregroundNode;
    foregroundNode.name = "foreground";
    foregroundNode.kind = px::vn::Stage::NodeKind::Image;
    foregroundNode.parent = "scene-root";
    foregroundNode.transform = {
        .x = 18.0f, .y = 30.0f, .scaleX = 1.25f, .scaleY = 0.75f,
        .rotation = 17.0f, .opacity = 220.0f / 255.0f};
    foregroundNode.z = 3;
    foregroundNode.order = 3;
    snapshot.stage.nodes.push_back(std::move(foregroundNode));

    px::vn::ParticleEmitterState savedEmitter;
    savedEmitter.name = "snow";
    savedEmitter.spec.preset = px::vn::ParticlePreset::Snow;
    savedEmitter.spec.seed = 73;
    savedEmitter.spec.rate = 42.0f;
    savedEmitter.spec.maxParticles = 200;
    savedEmitter.spec.z = -1;
    savedEmitter.spec.opacity = 0.7f;
    savedEmitter.spec.wind = 0.2f;
    savedEmitter.spec.speed = 0.9f;
    savedEmitter.spec.size = 1.1f;
    savedEmitter.ticks = 901;
    savedEmitter.tickRemainder = 0.25;
    snapshot.stage.particleEmitters.push_back(std::move(savedEmitter));
    auto& savedParticles = snapshot.stage.particleEmitters.back().spec;
    savedParticles.texture = "Content/Particles/snow-atlas.png";
    savedParticles.atlasColumns = 4;
    savedParticles.atlasRows = 2;
    savedParticles.atlasFrameCount = 8;
    savedParticles.spawnShape = px::vn::ParticleSpawnShape::Line;
    savedParticles.positionX = {0.1f, 0.9f};
    savedParticles.positionY = {0.0f, 0.2f};
    savedParticles.velocityX = {-10.0f, 10.0f};
    savedParticles.velocityY = {40.0f, 80.0f};
    savedParticles.lifetime = {3.0f, 6.0f};
    savedParticles.rotation = {-30.0f, 30.0f};
    savedParticles.angularVelocity = {-90.0f, 90.0f};
    savedParticles.scaleOverLifetime = {{0.0f, 0.5f}, {1.0f, 1.0f}};
    savedParticles.opacityOverLifetime = {{0.0f, 1.0f}, {1.0f, 0.0f}};
    savedParticles.colorOverLifetime = {
        {0.0f, {255, 255, 255, 255}}, {1.0f, {180, 210, 255, 0}}};
    savedParticles.gravity = 9.0f;
    savedParticles.variation = 0.25f;
    savedParticles.burst = 32;
    savedParticles.loop = false;
    savedParticles.duration = 2.0f;
    savedParticles.advanced = true;
    px::vn::Stage::SavedTween tween;
    tween.target = "alice";
    tween.spec.hasX = true;
    tween.spec.x = 24.0f;
    tween.elapsed = 0.2f;
    tween.duration = 0.6f;
    snapshot.stage.tweens.push_back(tween);
    snapshot.audio.music = {.path = "Content/Audio/theme.ogg",
                            .loop = true,
                            .playing = true,
                            .playbackFrame = 24000,
                            .paused = true,
                            .handle = 4242};
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
    snapshot.extensionState.push_back({
        "commercial-extension", "weather-memory", 3,
        px::Variant(px::VariantObject{
            {"seed", px::Variant(std::int64_t{42})},
            {"enabled", px::Variant(true)}})});
    snapshot.stage.motion = px::vn::Stage::SavedMotion{
        "Content/Motion/intro.pxmotion", 0.4, 1.25, true, true, false};
    Check(saves.Save(0, snapshot), "valid save should be written");
    const auto loaded = saves.Load(0);
    Check(loaded && loaded->pc == 7 && loaded->variables.at("affection") == 3, "valid save should round-trip");
    Check(loaded && loaded->stage.layers.size() == 1 && loaded->stage.layers.front().action &&
              loaded->stage.layers.front().action->preset == "shake-head" &&
              loaded->stage.layers.front().action->size == 10.0f &&
              loaded->stage.layers.front().action->loop == -1 &&
              loaded->stage.layers.front().actionElapsed == 1.5 &&
              loaded->stage.backgroundAction &&
              loaded->stage.backgroundAction->preset == "float" &&
              loaded->stage.backgroundAction->ease == "linear" &&
              loaded->stage.backgroundActionElapsed == 2.25,
          "Image layer and background actions survive save serialization");
    Check(loaded && loaded->stage.motion &&
              loaded->stage.motion->path == "Content/Motion/intro.pxmotion" &&
              loaded->stage.motion->position == 0.4 &&
              loaded->stage.motion->speed == 1.25 &&
              loaded->stage.motion->awaiting,
          "Stage motion position and await survive save serialization");
    Check(
        loaded && loaded->vm.state == px::vn::VMState::WaitingChoice && loaded->vm.callStack.size() == 1 && loaded->vm.choices.size() == 1 && loaded->dialogue.state.displayText == "Hel" &&
            loaded->dialogue.speedMs == 42 && loaded->typedVariables.at("route").TryGet<std::string>() && loaded->typedVariables.at("flags").AsObject() && loaded->routes.stack.size() == 2 && loaded->routes.modals.size() == 1 &&
            loaded->timelines.size() == 1 && loaded->timelines.front().awaiting && loaded->animationClips.size() == 1 && loaded->animationClips.front().name == "Custom/Save" && loaded->stage.backgroundFade == 0.45f && loaded->stage.ruleActive && loaded->stage.ruleMask == "Content/Transitions/clouds.png" && loaded->stage.ruleProgress == 0.35f && loaded->stage.ruleVague == 48 && loaded->stage.cameraX == 16.0f && loaded->stage.cameraY == -9.0f && loaded->stage.cameraZoom == 1.15f && loaded->stage.actors.size() == 1 && loaded->stage.layers.size() == 1 && loaded->stage.layers.front().scaleY == 0.75f && loaded->stage.layers.front().rotation == 17.0f &&
            loaded->stage.nodes.size() == 3 && loaded->stage.nodes.front().name == "scene-root" && loaded->stage.nodes.front().children.size() == 2 && loaded->stage.particleEmitters.size() == 1 && loaded->stage.particleEmitters.front().spec.preset == px::vn::ParticlePreset::Snow && loaded->stage.particleEmitters.front().spec.advanced && loaded->stage.particleEmitters.front().spec.texture == "Content/Particles/snow-atlas.png" && loaded->stage.particleEmitters.front().spec.atlasFrameCount == 8 && loaded->stage.particleEmitters.front().spec.burst == 32 && loaded->stage.particleEmitters.front().spec.colorOverLifetime.size() == 2 && loaded->stage.particleEmitters.front().ticks == 901 && loaded->stage.particleEmitters.front().tickRemainder == 0.25 &&
            loaded->stage.tweens.size() == 1 && loaded->audio.music.playbackFrame == 24000 && loaded->audio.music.paused && loaded->audio.music.handle == 4242 && loaded->scriptPending.size() == 2 && loaded->scriptPending.front().yieldIndex == 1 && loaded->scriptPending.back().waitKind == "screen-effect" && loaded->scriptPending.back().handle == 91 && loaded->scriptActions.size() == 1 && loaded->scriptActions.front().yieldIndex == 2 &&
            loaded->ui.behavior.fibers.size() == 1 && loaded->ui.behavior.fibers.front().current == behaviorDelay && loaded->ui.behavior.fibers.front().signalArguments.at("position").TryGet<px::Vec2>() && loaded->ui.behavior.actions.size() == 1 &&
            loaded->ui.behavior.actions.front().providerHandle == 73 &&
            loaded->ui.animation && loaded->ui.animation->position == 0.42f &&
            loaded->ui.animation->parameters.at("selected").TryGet<bool>() &&
            loaded->ui.visualState && loaded->ui.visualState->groups.size() == 1 &&
            loaded->ui.visualState->groups.front().state == "hover" &&
            loaded->ui.visualState->groups.front().animationPosition == 0.08f &&
            loaded->extensionState.size() == 1 &&
            loaded->extensionState.front().version == 3 &&
            loaded->extensionState.front().state.AsObject() &&
            loaded->extensionState.front().state.AsObject()->at("seed") ==
                px::Variant(std::int64_t{42}),
        "current save schema should preserve exact VM, stage, audio, script, UI, and variable state"
    );

    const auto currentJson = px::progress::LoadJson(saves.SlotPath(0), nullptr);
    Check(currentJson && currentJson->value("schemaRevision", 0) == 4 &&
              currentJson->contains("integrityHash") &&
              currentJson->contains("anchor") && currentJson->contains("state") &&
              (*currentJson)["state"].contains("scriptPending") &&
              (*currentJson)["state"].contains("scriptActions") &&
              (*currentJson)["state"].contains("extensionState") &&
              (*currentJson)["state"].contains("ui"),
          "current saves must use an authenticated v2 envelope and complete runtime state");
    if (currentJson) {
        const auto rehash = [](px::progress::Json value) {
            value.erase("integrityHash");
            value["integrityHash"] = px::crypto::Sha256Hex(value.dump());
            return value.dump();
        };
        for (const auto& [pointer, value] : std::vector<std::pair<std::string, px::progress::Json>>{
                 {"/state/values/overflow", std::uint64_t{9223372036854775808ull}},
                 {"/saveVersion", std::uint64_t{4294967297ull}},
                 {"/state/pc", std::uint64_t{4294967296ull}},
                 {"/state/vm/pc", std::uint64_t{4294967296ull}},
                 {"/state/timestamp", -1}, {"/state/playtimeMs", -1},
                 {"/state/variables/affection", std::uint64_t{4294967296ull}}}) {
            auto invalid = *currentJson;
            invalid[px::progress::Json::json_pointer(pointer)] = value;
            Check(!px::progress::ParseSaveEnvelope(rehash(std::move(invalid))),
                  "authenticated integer overflow must fail rather than narrow or wrap");
        }
        const auto validText = currentJson->dump();
        const auto duplicate = std::string("{\"format\":\"PrismatiXSave\",") + validText.substr(1);
        Check(!px::progress::ParseSaveEnvelope(duplicate),
              "duplicate envelope keys must be rejected before canonical hashing");
        {
            std::ofstream fixture(saves.SlotPath(6), std::ios::binary);
            fixture << "PRISMATIX-PERSIST\n" << duplicate;
        }
        Check(!saves.Load(6), "persistence reader must not erase duplicate-key evidence");
        auto deep = *currentJson;
        px::progress::Json nested = nullptr;
        for (int depth = 0; depth < 130; ++depth) nested = px::progress::Json::array({nested});
        deep["extra"] = std::move(nested);
        Check(!px::progress::ParseSaveEnvelope(rehash(std::move(deep))),
              "save JSON depth over 128 must fail before materializing the DOM");
        Check(!px::progress::ParseSaveEnvelope("{\"format\":3,\"schemaRevision\":4}") &&
                  !px::progress::ParseSaveEnvelope("{\"format\":\"PrismatiXSave\",\"schemaRevision\":\"four\"}"),
              "wrong header types return failure without escaping exceptions");
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
    if (const char* output = SDL_getenv("PRISMATIX_TEST_INSPECT_FIXTURE")) {
        const std::filesystem::path root(output);
        auto exported = snapshot;
        exported.timestamp = 9007199254740993ull;
        exported.playtimeMs = (std::numeric_limits<std::uint64_t>::max)();
        exported.typedVariables["floatZero"] = px::Variant(0.0);
        exported.typedVariables["floatOne"] = px::Variant(1.0);
        const auto currentKey = px::progress::DeriveGameDataPersistenceKey(exported.gameId);
        const auto legacyKey = px::progress::DeriveLegacyPersistenceKey("fixture-legacy-secret");
        for (const auto& [name, key] : std::vector<std::pair<std::string, const px::crypto::Key*>>{
                 {"plaintext", nullptr}, {"current", &currentKey}, {"legacy", &legacyKey}}) {
            px::progress::SaveSystem writer;
            writer.Configure((root / name).string(), key);
            Check(writer.Save(0, exported), "native inspector fixture writer succeeds");
        }
        const px::progress::Json manifest{
            {"format", "PrismatiXPackageManifest"}, {"schemaRevision", 2},
            {"engineVersion", "0.2.0"}, {"gameId", exported.gameId}, {"title", "Inspector fixture"},
            {"contentVersion", exported.contentVersion}, {"packageFingerprint", exported.packageFingerprint},
            {"saveVersion", 1}, {"graphicsTier", "basic"},
            {"resolution", {{"width", 1280}, {"height", 720}}},
            {"entry", {{"runtimeIr", "Runtime/main.pxir"}, {"sourceMap", "Runtime/main.pxmap"}, {"route", ""}}},
            {"routes", px::progress::Json::array()},
            {"archives", px::progress::Json::array({{{"file", "Content.pdx"}, {"group", "content"}, {"optional", false}}})},
            {"saveMigrations", px::progress::Json::array()}, {"extensions", px::progress::Json::array()},
            {"customEffects", px::progress::Json::array()}, {"encryption", {{"enabled", false}, {"archiveKey", ""}}},
            {"persistence", {{"dataId", exported.gameId}, {"keyId", "game-data-v1"},
                {"legacyReaders", px::progress::Json::array({{{"keyId", "legacy-archive-v1"}, {"secret", "fixture-legacy-secret"}}})}}}};
        std::ofstream file(root / "manifest.json", std::ios::binary);
        file << manifest.dump();
    }
}

std::string ReadBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
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
    px::VariantObject score;
    score.emplace("score", px::Variant(12.5));
    px::VariantArray flags;
    flags.emplace_back(true);
    flags.emplace_back();
    flags.emplace_back(std::move(score));
    px::VariantObject routeStateObject;
    routeStateObject.emplace("route", px::Variant("alice"));
    routeStateObject.emplace("flags", px::Variant(std::move(flags)));
    const px::Variant routeState(std::move(routeStateObject));
    profile.SetVariable("routeState", routeState.Clone());
    profile.UnlockScene("scene.after-story");
    profile.UnlockCG("cg.sunset");
    profile.UnlockMusic("music.ending");
    Check(px::progress::SaveGlobalProfile(profile, profilePath, nullptr), "global profile should be written with the current schema");

    px::progress::GlobalProfile loadedProfile;
    Check(px::progress::LoadGlobalProfile(loadedProfile, profilePath, nullptr), "global profile should load with the current schema");
    Check(
        loadedProfile.HasSeen("scene.intro#line-1") && loadedProfile.HasChoiceSeen("choice.intro#option-a") && loadedProfile.ClearCount() == 1 && loadedProfile.RouteCleared("route.alice") &&
            loadedProfile.Variable("affection") && loadedProfile.Variable("affection")->TryGet<std::int64_t>() && *loadedProfile.Variable("affection")->TryGet<std::int64_t>() == 7 && loadedProfile.Variable("routeState") && *loadedProfile.Variable("routeState") == routeState && loadedProfile.SceneUnlocked("scene.after-story") && loadedProfile.CGUnlocked("cg.sunset") && loadedProfile.MusicUnlocked("music.ending"),
        "global profile should preserve seen state, clears, scalar/composite persistent variables, and unlocks"
    );

    const auto unchangedAfterRejectedApply = [&]() {
        return loadedProfile.HasSeen("scene.intro#line-1") &&
               loadedProfile.ClearCount() == 1 &&
               loadedProfile.Variable("affection") &&
               loadedProfile.Variable("affection")->TryGet<std::int64_t>() &&
               *loadedProfile.Variable("affection")->TryGet<std::int64_t>() == 7 &&
               loadedProfile.Variable("routeState") &&
               *loadedProfile.Variable("routeState") == routeState;
    };
    px::progress::Json invalidProfile = loadedProfile.ToJson();
    invalidProfile["vars"]["nonFinite"] =
        std::numeric_limits<double>::infinity();
    Check(!loadedProfile.ApplyJson(invalidProfile) && unchangedAfterRejectedApply(),
          "invalid composite profile values must be rejected transactionally");

    px::progress::Json deepValue = nullptr;
    for (int depth = 0; depth < 34; ++depth) {
        deepValue = px::progress::Json::array({std::move(deepValue)});
    }
    px::progress::Json deepProfile = loadedProfile.ToJson();
    deepProfile["vars"]["tooDeep"] = std::move(deepValue);
    Check(!loadedProfile.ApplyJson(deepProfile) && unchangedAfterRejectedApply(),
          "excessively deep profile values must be rejected transactionally");

    px::progress::Json wideProfile = loadedProfile.ToJson();
    px::progress::Json tooWide = px::progress::Json::array();
    tooWide.get_ref<px::progress::Json::array_t&>().resize(100'001);
    wideProfile["vars"]["tooWide"] = std::move(tooWide);
    Check(!loadedProfile.ApplyJson(wideProfile) && unchangedAfterRejectedApply(),
          "excessively wide profile values must be rejected transactionally");

    px::progress::GlobalProfile unsupportedProfile;
    unsupportedProfile.SetVariable(
        "editorColor", px::Variant(px::Color{255, 0, 0, 255}));
    Check(!px::progress::SaveGlobalProfile(unsupportedProfile, profilePath, nullptr),
          "non-JSON Variant values must fail profile serialization");
    px::progress::GlobalProfile preservedProfile;
    Check(px::progress::LoadGlobalProfile(preservedProfile, profilePath, nullptr) &&
              preservedProfile.Variable("routeState") &&
              *preservedProfile.Variable("routeState") == routeState,
          "a rejected profile save must preserve the previous file");

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
    settings.masterVolume = 73;
    settings.bgmVolume = 64;
    settings.voiceVolume = 96;
    settings.masterMuted = true;
    settings.voiceMuted = true;
    settings.duckBgmOnVoice = false;
    settings.continueVoiceOnAdvance = true;
    settings.voiceProfileVolumes["rnd"] = 63;
    settings.voiceProfileMuted["rnd"] = true;
    settings.language = "ja-JP";
    settings.textScale = 1.25f;
    settings.highContrast = true;
    settings.reducedMotion = true;
    settings.selfVoicing = true;
    const int f11 = px::Input::KeyCode("F11").value_or(0);
    px::Input sourceInput;
    Check(f11 > 0 &&
              sourceInput.RebindKey("quick-save", f11).status ==
                  px::KeyRebindStatus::Applied,
          "input map can author a portable keyboard settings override");
    Check(sourceInput.SetWheelBinding("advance", px::WheelDirection::None) &&
              sourceInput.SetWheelBinding("backlog", px::WheelDirection::Up, 1.5f),
          "input map can author wheel settings overrides");
    const int f10 = px::Input::KeyCode("F10").value_or(0);
    Check(f10 > 0 && sourceInput.RegisterAction("host.later") &&
              sourceInput.BindKey("host.later", f10) &&
              sourceInput.SetWheelBinding("host.later",
                                          px::WheelDirection::Down, 2.0f),
          "a host action can save keyboard and wheel bindings before registration on reload");
    Check(settings.SaveWithInputBindings(settingsPath, sourceInput, nullptr),
          "a host saves keyboard and wheel bindings with the current schema");

    px::progress::GameSettings loadedSettings;
    px::Input restoredInput;
    Check(loadedSettings.LoadWithInputBindings(settingsPath, restoredInput,
                                                nullptr) ==
              px::progress::PersistLoadStatus::Loaded,
          "a host loads settings and input bindings together");
    Check(
        loadedSettings.masterVolume == 73 && loadedSettings.bgmVolume == 64 && loadedSettings.voiceVolume == 96 && loadedSettings.masterMuted && loadedSettings.voiceMuted && !loadedSettings.duckBgmOnVoice && loadedSettings.continueVoiceOnAdvance && loadedSettings.voiceProfileVolumes.at("rnd") == 63 && loadedSettings.voiceProfileMuted.at("rnd") && loadedSettings.language == "ja-JP" && loadedSettings.textScale == 1.25f && loadedSettings.highContrast && loadedSettings.reducedMotion && loadedSettings.selfVoicing && loadedSettings.keyboardBindings.at("quick-save") == std::vector<int>{f11} && loadedSettings.wheelBindings.at("backlog").threshold == 1.5f,
        "game settings should preserve audio, locale, accessibility, keyboard, and wheel overrides"
    );
    Check(restoredInput.KeyBindings("quick-save") ==
                  std::vector<int>{f11},
          "a saved shortcut is applied by a fresh runtime input map");
    Check(restoredInput.GetWheelBinding("advance")->direction ==
                  px::WheelDirection::None &&
              restoredInput.GetWheelBinding("backlog")->threshold == 1.5f,
          "saved wheel overrides apply to a fresh runtime input map");
    Check(restoredInput.RegisterAction("host.later") &&
              restoredInput.KeyBindings("host.later") ==
                  std::vector<int>{f10} &&
              restoredInput.GetWheelBinding("host.later")->direction ==
                  px::WheelDirection::Down,
          "actions registered after settings load receive both saved devices");

    const auto beforeInvalidKeys = restoredInput.CaptureKeyboardBindings();
    const auto beforeInvalidWheel = restoredInput.CaptureWheelBindings();
    px::progress::Json invalidScancode{
        {"format", "PrismatiXSettings"}, {"schemaRevision", 2},
        {"keyboardBindings", {{"quick-save", std::vector<int>{-1}}}},
        {"wheelBindings", {{"backlog", {{"direction", "down"},
                                          {"threshold", 3.0}}}}}};
    Check(px::progress::SaveJson(settingsPath, invalidScancode, nullptr) &&
              loadedSettings.LoadWithInputBindings(settingsPath, restoredInput,
                                                   nullptr) ==
                  px::progress::PersistLoadStatus::Failed &&
              restoredInput.CaptureKeyboardBindings() == beforeInvalidKeys &&
              restoredInput.CaptureWheelBindings().at("backlog").threshold ==
                  beforeInvalidWheel.at("backlog").threshold &&
              restoredInput.CaptureWheelBindings().at("backlog").direction ==
                  beforeInvalidWheel.at("backlog").direction &&
              loadedSettings.keyboardBindings.at("quick-save") ==
                  std::vector<int>{f11},
          "invalid saved keys leave both input devices and settings unchanged");
    auto invalidCombined = loadedSettings;
    invalidCombined.keyboardBindings["quick-save"] = {f10};
    invalidCombined.wheelBindings["backlog"] = {
        px::WheelDirection::Up, -1.0f};
    Check(!invalidCombined.ApplyInputBindings(restoredInput) &&
              restoredInput.CaptureKeyboardBindings() == beforeInvalidKeys &&
              restoredInput.GetWheelBinding("backlog")->threshold == 1.5f,
          "a rejected wheel override cannot partially apply valid keyboard changes");

    px::progress::Json invalidBindings{
        {"format", "PrismatiXSettings"}, {"schemaRevision", 2},
        {"keyboardBindings", {{"quick-save", "not-an-array"}}}};
    Check(px::progress::SaveJson(settingsPath, invalidBindings, nullptr),
          "invalid keyboard settings fixture should be written");
    Check(!loadedSettings.Load(settingsPath, nullptr) &&
              loadedSettings.keyboardBindings.at("quick-save") ==
                  std::vector<int>{f11},
          "invalid keyboard binding type must not partially replace settings");
    px::progress::Json invalidWheel{
        {"format", "PrismatiXSettings"}, {"schemaRevision", 2},
        {"wheelBindings", {{"backlog", {{"direction", "none"},
                                         {"threshold", 1.0}}}}}};
    Check(px::progress::SaveJson(settingsPath, invalidWheel, nullptr) &&
              !loadedSettings.Load(settingsPath, nullptr) &&
              loadedSettings.wheelBindings.at("backlog").threshold == 1.5f,
          "invalid wheel binding must not partially replace settings");
    px::progress::Json settingsWithoutBindings{
        {"format", "PrismatiXSettings"}, {"schemaRevision", 2}};
    Check(px::progress::SaveJson(settingsPath, settingsWithoutBindings, nullptr),
          "older settings without keyboard overrides should be written");
    Check(loadedSettings.Load(settingsPath, nullptr) &&
              loadedSettings.keyboardBindings.empty() &&
              loadedSettings.wheelBindings.empty(),
          "settings without input overrides restore engine defaults");

    px::progress::Json oldSettingsWithKeys{
        {"format", "PrismatiXSettings"}, {"schemaRevision", 2},
        {"keyboardBindings", {{"quick-save", std::vector<int>{f11}}}}};
    Check(px::progress::SaveJson(settingsPath, oldSettingsWithKeys, nullptr) &&
              loadedSettings.Load(settingsPath, nullptr) &&
              loadedSettings.wheelBindings.empty(),
          "older keyboard settings without wheel bindings load safely");
    px::Input migratedInput;
    Check(loadedSettings.ApplyInputBindings(migratedInput) &&
              migratedInput.GetWheelBinding("advance")->direction ==
                  px::WheelDirection::Down &&
              migratedInput.GetWheelBinding("backlog")->direction ==
                  px::WheelDirection::Up,
          "missing wheel settings retain the runtime's authored defaults");
    Check(migratedInput.SetWheelBinding("advance", px::WheelDirection::Up,
                                        2.0f),
          "an old settings user can rebind the wheel");
    Check(loadedSettings.SaveWithInputBindings(settingsPath, migratedInput,
                                               nullptr),
          "old settings can save a new wheel override");
    px::progress::GameSettings migratedSettings;
    px::Input reopenedInput;
    Check(migratedSettings.LoadWithInputBindings(settingsPath, reopenedInput,
                                                nullptr) ==
              px::progress::PersistLoadStatus::Loaded &&
              reopenedInput.GetWheelBinding("advance")->direction ==
                  px::WheelDirection::Up &&
              reopenedInput.GetWheelBinding("advance")->threshold == 2.0f,
          "wheel override persists after reopening migrated settings");

    for (const px::progress::Json& malformedHeader : {
             px::progress::Json{{"format", 42}, {"schemaRevision", 2}},
             px::progress::Json{{"format", "PrismatiXSettings"},
                                {"schemaRevision", "2"}}}) {
        Check(px::progress::SaveJson(settingsPath, malformedHeader, nullptr),
              "malformed settings header fixture should be written");
        const std::string originalBytes = ReadBytes(settingsPath);
        Check(migratedSettings.LoadWithFallback(settingsPath, nullptr) ==
                  px::progress::PersistLoadStatus::Failed &&
                  !migratedSettings.SavePreservingExisting(settingsPath,
                                                           nullptr) &&
                  ReadBytes(settingsPath) == originalBytes &&
                  migratedSettings.wheelBindings.at("advance").threshold ==
                      2.0f,
              "malformed settings headers fail without replacing user data");
    }

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

    const auto sparseMigration = [](std::string_view)
        -> std::optional<std::string> {
        return R"({
          "format":"PrismatiXSaveMigration","schemaRevision":2,
          "id":"chapter-1-5-to-2",
          "from":{"contentVersion":"chapter-1","saveVersion":1},
          "to":{"contentVersion":"chapter-2","saveVersion":2},
          "anchor":{"policy":"map-or-preserve","mappings":[
            {"from":{"runtimeDocumentId":"document-old","sourceId":"line-old","operationId":"operation-old"},
             "to":{"runtimeDocumentId":"document-new","sourceId":"line-new","operationId":"operation-new"}},
            {"from":{"runtimeDocumentId":"document-new","sourceId":"line-new","operationId":"operation-new"},
             "to":{"runtimeDocumentId":"wrong","sourceId":"wrong","operationId":"wrong"}}
          ]},"operations":[]
        })";
    };
    auto sparseMapped = px::progress::MigrateSaveSnapshot(
        source, {"commercial-vn", std::string(64, 'b'), "chapter-2", 2},
        missingAnchor, sparseMigration);
    Check(sparseMapped &&
              sparseMapped.Value().anchor.runtimeDocumentId == "document-new" &&
              sparseMapped.Value().anchor.operationId == "operation-new",
          "sparse mappings apply once against the original anchor, without chaining");

    auto unchangedSource = source;
    unchangedSource.anchor = {"still-present", "line-still", "operation-still"};
    auto sparsePreserved = px::progress::MigrateSaveSnapshot(
        unchangedSource,
        {"commercial-vn", std::string(64, 'b'), "chapter-2", 2},
        missingAnchor, sparseMigration);
    Check(sparsePreserved &&
              sparsePreserved.Value().anchor.runtimeDocumentId == "still-present" &&
              sparsePreserved.Value().anchor.operationId == "operation-still",
          "sparse mappings preserve unlisted anchor identities");

    auto duplicateMapping = px::progress::MigrateSaveSnapshot(
        source, {"commercial-vn", std::string(64, 'b'), "chapter-2", 2},
        missingAnchor,
        [](std::string_view) -> std::optional<std::string> {
            return R"({
              "format":"PrismatiXSaveMigration","schemaRevision":2,
              "id":"chapter-1-5-to-2",
              "from":{"contentVersion":"chapter-1","saveVersion":1},
              "to":{"contentVersion":"chapter-2","saveVersion":2},
              "anchor":{"policy":"map-or-preserve","mappings":[
                {"from":{"runtimeDocumentId":"document-old","sourceId":"line-old","operationId":"operation-old"},
                 "to":{"runtimeDocumentId":"document-new","sourceId":"line-new","operationId":"operation-new"}},
                {"from":{"runtimeDocumentId":"document-old","sourceId":"line-old","operationId":"operation-old"},
                 "to":{"runtimeDocumentId":"other","sourceId":"other","operationId":"other"}}
              ]},"operations":[]
            })";
        });
    Check(!duplicateMapping && source.anchor.operationId == "operation-old",
          "sparse mappings reject two destinations for one source anchor");
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

void TestAwaitedWorkValidation() {
    px::progress::SaveSnapshot snapshot;
    snapshot.vm.state = px::vn::VMState::WaitingExternal;
    px::script::PendingActionState action;
    action.id = 7;
    action.invocation.action = "test.async";
    snapshot.scriptActions.push_back(std::move(action));
    const auto actionOnly = px::progress::InspectAwaitedWork(snapshot);
    Check(actionOnly.action && !actionOnly.command && !actionOnly.timeline &&
              px::progress::HasConsistentAwaitState(snapshot),
          "an action-only checkpoint is a valid external await state");

    snapshot.scriptActions.clear();
    Check(!px::progress::HasConsistentAwaitState(snapshot),
          "WaitingExternal without restorable work is rejected");
    snapshot.vm.state = px::vn::VMState::WaitingClick;
    Check(px::progress::HasConsistentAwaitState(snapshot),
          "non-external VM state without awaited work is valid");

    px::animation::PlaybackState timeline;
    timeline.playing = true;
    timeline.awaiting = true;
    snapshot.timelines.push_back(timeline);
    Check(!px::progress::HasConsistentAwaitState(snapshot),
          "awaited timeline requires WaitingExternal");
    snapshot.vm.state = px::vn::VMState::WaitingExternal;
    Check(px::progress::HasConsistentAwaitState(snapshot),
          "awaited timeline and WaitingExternal agree");
    snapshot.timelines.clear();
    snapshot.stage.motion = px::vn::Stage::SavedMotion{
        "Content/Motion/intro.pxmotion", 0.4, 1.25, true, true, false};
    Check(px::progress::InspectAwaitedWork(snapshot).stageMotion &&
              px::progress::HasConsistentAwaitState(snapshot),
          "awaited Stage motion is restorable external work");
    snapshot.vm.state = px::vn::VMState::WaitingClick;
    Check(!px::progress::HasConsistentAwaitState(snapshot),
          "awaited Stage motion requires WaitingExternal");
}

void TestPersistenceKeyUpgradeAndFailurePreservation() {
    px::test::TempDirectory temp("persistence-key-upgrade");
    const auto profilePath = temp.path / "profile.dat";
    const auto settingsPath = temp.path / "config.dat";
    const auto corruptPath = temp.path / "protected.dat";
    const auto stableKey =
        px::progress::DeriveGameDataPersistenceKey("commercial-game");
    const auto stableKeyAgain =
        px::progress::DeriveGameDataPersistenceKey("commercial-game");
    const auto otherStableKey =
        px::progress::DeriveGameDataPersistenceKey("different-game");
    const auto legacyKey =
        px::progress::DeriveLegacyPersistenceKey("content-fingerprint-v1");
    const auto wrongKey =
        px::progress::DeriveLegacyPersistenceKey("unavailable-key");
    Check(stableKey == stableKeyAgain && stableKey != otherStableKey &&
              stableKey != legacyKey,
          "game-data persistence identity is stable and isolated from content keys");
    const std::vector<px::crypto::Key> fallbackKeys{legacyKey};

    px::progress::GlobalProfile legacyProfile;
    legacyProfile.MarkSeen("chapter-1#line-1");
    legacyProfile.SetVariable("route", px::Variant("alice"));
    Check(px::progress::SaveGlobalProfile(
              legacyProfile, profilePath.string(), &legacyKey),
          "legacy-key profile fixture is written");
    const std::string legacyProfileBytes = ReadBytes(profilePath);
    px::progress::GlobalProfile loadedProfile;
    Check(px::progress::LoadGlobalProfileWithFallback(
              loadedProfile, profilePath.string(), &stableKey,
              fallbackKeys) == px::progress::PersistLoadStatus::Loaded &&
              loadedProfile.HasSeen("chapter-1#line-1"),
          "current reader loads a legacy-key profile before migration");
    loadedProfile.MarkSeen("chapter-2#line-1");
    Check(px::progress::SaveGlobalProfilePreservingExisting(
              loadedProfile, profilePath.string(), &stableKey,
              fallbackKeys),
          "validated legacy profile is promoted to the stable key");
    const auto profileBackup =
        std::filesystem::path(profilePath.string() + ".legacy.bak");
    Check(std::filesystem::is_regular_file(profileBackup) &&
              ReadBytes(profileBackup) == legacyProfileBytes,
          "legacy profile bytes are retained before stable-key promotion");
    px::progress::GlobalProfile promotedProfile;
    Check(px::progress::LoadGlobalProfile(
              promotedProfile, profilePath.string(), &stableKey) &&
              promotedProfile.HasSeen("chapter-1#line-1") &&
              promotedProfile.HasSeen("chapter-2#line-1"),
          "promoted profile reloads with the stable key and preserves state");

    px::progress::GameSettings legacySettings;
    legacySettings.language = "ja-JP";
    legacySettings.textSpeedMs = 19;
    Check(legacySettings.Save(settingsPath.string(), &legacyKey),
          "legacy-key settings fixture is written");
    const std::string legacySettingsBytes = ReadBytes(settingsPath);
    px::progress::GameSettings loadedSettings;
    Check(loadedSettings.LoadWithFallback(
              settingsPath.string(), &stableKey, fallbackKeys) ==
              px::progress::PersistLoadStatus::Loaded &&
              loadedSettings.language == "ja-JP" &&
              loadedSettings.textSpeedMs == 19,
          "current reader loads legacy-key settings");
    loadedSettings.autoWaitMs = 875;
    Check(loadedSettings.SavePreservingExisting(
              settingsPath.string(), &stableKey, fallbackKeys) &&
              ReadBytes(settingsPath.string() + ".legacy.bak") ==
                  legacySettingsBytes,
          "settings promotion retains the exact legacy bytes");
    px::progress::GameSettings promotedSettings;
    Check(promotedSettings.Load(settingsPath.string(), &stableKey) &&
              promotedSettings.language == "ja-JP" &&
              promotedSettings.autoWaitMs == 875,
          "promoted settings reload with the stable key");

    px::progress::GlobalProfile protectedProfile;
    protectedProfile.MarkSeen("must-survive");
    Check(px::progress::SaveGlobalProfile(
              protectedProfile, corruptPath.string(), &wrongKey),
          "wrong-key protection fixture is written");
    const std::string protectedBytes = ReadBytes(corruptPath);
    px::progress::GlobalProfile replacement;
    replacement.MarkSeen("replacement");
    Check(px::progress::LoadGlobalProfileWithFallback(
              replacement, corruptPath.string(), &stableKey,
              fallbackKeys) == px::progress::PersistLoadStatus::Failed &&
              !px::progress::SaveGlobalProfilePreservingExisting(
                  replacement, corruptPath.string(), &stableKey,
                  fallbackKeys) &&
              ReadBytes(corruptPath) == protectedBytes,
          "an existing file with an unavailable key is never overwritten");

    px::progress::SaveSnapshot snapshot;
    snapshot.gameId = "commercial-game";
    snapshot.packageFingerprint = std::string(64, '1');
    snapshot.contentVersion = "1";
    snapshot.saveVersion = 1;
    snapshot.anchor = {"runtime-main", "line-1", "operation-1"};
    snapshot.scriptPath = "Runtime/main.pxir";
    snapshot.vm.scriptPath = snapshot.scriptPath;
    snapshot.chapter = "chapter-1";
    px::progress::SaveSystem legacySaves;
    legacySaves.Configure((temp.path / "Save").string(), &legacyKey);
    Check(legacySaves.Save(1, snapshot),
          "legacy-key slot fixture is written");
    const std::string legacySlotBytes = ReadBytes(legacySaves.SlotPath(1));
    px::progress::SaveSystem currentSaves;
    currentSaves.Configure((temp.path / "Save").string(), &stableKey,
                           fallbackKeys);
    const auto loadedSlot = currentSaves.Load(1);
    Check(loadedSlot && loadedSlot->chapter == "chapter-1",
          "current save reader loads the legacy-key slot");
    snapshot.chapter = "chapter-2";
    Check(currentSaves.Save(1, snapshot) &&
              ReadBytes(currentSaves.SlotPath(1) + ".legacy.bak") ==
                  legacySlotBytes &&
              currentSaves.Load(1) &&
              currentSaves.Load(1)->chapter == "chapter-2",
          "slot promotion backs up legacy bytes and writes the stable key");
}

void TestPackagedUserDataMigration() {
    px::test::TempDirectory temp("packaged-user-data");
    const auto packageRoot = temp.path / "Package";
    const auto oldSave = packageRoot / "Save";
    const auto userRoot = temp.path / "UserData";
    std::filesystem::create_directories(oldSave);
    const auto key = px::progress::DeriveGameDataPersistenceKey("user-data-test");
    px::progress::GameSettings legacySettings;
    legacySettings.language = "ja-JP";
    Check(legacySettings.Save(px::PathToUtf8(oldSave / "config.dat"), &key),
          "portable settings fixture is written");
    px::progress::GlobalProfile legacyProfile;
    legacyProfile.MarkSeen("legacy-line");
    Check(px::progress::SaveGlobalProfile(
              legacyProfile, px::PathToUtf8(oldSave / "profile.dat"), &key),
          "portable profile fixture is written");
    {
        std::ofstream slot(oldSave / "save_0.pxsav", std::ios::binary);
        slot << "portable-slot";
        std::ofstream backup(oldSave / "config.dat.legacy.bak", std::ios::binary);
        backup << "portable-backup";
        std::ofstream unknown(oldSave / "notes.txt", std::ios::binary);
        unknown << "must-stay-behind";
    }
    const auto imported = px::progress::ImportLegacyPortableSaves(
        packageRoot, userRoot);
    Check(imported && imported.Value() == 4,
          "only known portable save files and backups are imported");
    Check(ReadBytes(userRoot / "Save/config.dat") == ReadBytes(oldSave / "config.dat") &&
          ReadBytes(userRoot / "Save/profile.dat") == ReadBytes(oldSave / "profile.dat") &&
          ReadBytes(userRoot / "Save/save_0.pxsav") == "portable-slot" &&
          ReadBytes(userRoot / "Save/config.dat.legacy.bak") == "portable-backup" &&
          !std::filesystem::exists(userRoot / "Save/notes.txt") &&
          std::filesystem::exists(oldSave / "notes.txt"),
          "migration copies exact bytes without deleting portable originals");
    px::progress::GameSettings loaded;
    Check(loaded.Load(px::PathToUtf8(userRoot / "Save/config.dat"), &key) &&
          loaded.language == "ja-JP",
          "migrated encrypted settings remain readable at their new path");
    loaded.language = "zh-TW";
    Check(loaded.Save(px::PathToUtf8(userRoot / "Save/config.dat"), &key),
          "new user settings can be written independently of the package");
    const auto repeated = px::progress::ImportLegacyPortableSaves(
        packageRoot, userRoot);
    px::progress::GameSettings reloaded;
    Check(repeated && repeated.Value() == 0 &&
          reloaded.Load(px::PathToUtf8(userRoot / "Save/config.dat"), &key) &&
          reloaded.language == "zh-TW",
          "one-time migration never restores stale portable settings");

    const auto invalidPackage = temp.path / "InvalidPackage";
    std::filesystem::create_directories(invalidPackage);
    {
        std::ofstream nonDirectory(invalidPackage / "Save", std::ios::binary);
        nonDirectory << "not-a-directory";
    }
    Check(!px::progress::ImportLegacyPortableSaves(
              invalidPackage, temp.path / "InvalidUserData"),
          "a non-directory legacy Save path fails closed");

    const auto unicodeSettings = temp.path / px::PathFromUtf8("存檔") / "config.dat";
    Check(legacySettings.Save(px::PathToUtf8(unicodeSettings), &key),
          "UTF-8 user-data path can be written");
    px::progress::GameSettings unicodeLoaded;
    Check(unicodeLoaded.Load(px::PathToUtf8(unicodeSettings), &key) &&
          unicodeLoaded.language == "ja-JP",
          "UTF-8 user-data path round-trips without Windows ACP loss");
    const auto unicodeProfile = unicodeSettings.parent_path() / "profile.dat";
    px::progress::GlobalProfile unicodeProfileValue;
    unicodeProfileValue.MarkSeen("unicode-path-line");
    Check(px::progress::SaveGlobalProfile(
              unicodeProfileValue, px::PathToUtf8(unicodeProfile), &key),
          "UTF-8 profile path can be written");
    px::progress::GlobalProfile reloadedProfile;
    Check(px::progress::LoadGlobalProfile(
              reloadedProfile, px::PathToUtf8(unicodeProfile), &key) &&
          reloadedProfile.HasSeen("unicode-path-line"),
          "UTF-8 profile path round-trips");
    px::progress::SaveSnapshot snapshot;
    snapshot.gameId = "user-data-test";
    snapshot.packageFingerprint = std::string(64, 'a');
    snapshot.contentVersion = "1";
    snapshot.saveVersion = 1;
    snapshot.anchor = {"runtime-main", "line-1", "operation-1"};
    snapshot.scriptPath = "Runtime/main.pxir";
    snapshot.vm.scriptPath = snapshot.scriptPath;
    px::progress::SaveSystem unicodeSlots;
    unicodeSlots.Configure(px::PathToUtf8(unicodeSettings.parent_path()), &key);
    Check(unicodeSlots.Save(0, snapshot) && unicodeSlots.Load(0).has_value(),
          "UTF-8 save-slot path round-trips");
    Check(unicodeSlots.Delete(0) && !unicodeSlots.Load(0).has_value(),
          "UTF-8 save-slot path can be deleted without ACP conversion");
}

void TestOptionalPackPreferenceAtomicWrite() {
    px::test::TempDirectory temp("optional-pack-preference");
    const auto preference = temp.path / "optional-pack.txt";
    const auto first = px::progress::WriteEnabledOptionalPackId(
        temp.path, "adult");
    Check(first && ReadBytes(preference) == "adult\n",
          "optional pack preference is written as a complete file");
    const auto replacement = px::progress::WriteEnabledOptionalPackId(
        temp.path, "voice");
    const auto selected = px::progress::ReadEnabledOptionalPackId(temp.path);
    Check(replacement && selected && selected.Value() == "voice" &&
              ReadBytes(preference) == "voice\n",
          "replacing an optional pack preference publishes complete bytes");
    Check(!px::progress::WriteEnabledOptionalPackId(temp.path, "INVALID") &&
              ReadBytes(preference) == "voice\n",
          "rejected optional pack IDs preserve the previous preference");
    bool staged = false;
    for (const auto& entry : std::filesystem::directory_iterator(temp.path))
        staged |= entry.path().filename().string().starts_with(
            "optional-pack.txt.tmp.");
    Check(!staged, "successful preference replacement leaves no staging file");
}


}  // namespace

int main() {
    Run("Archive_BoundsAndRoundTrip", TestArchiveBoundsAndRoundTrip);
    Run("Save_RejectsInvalidAndRoundTrips", TestSaveValidation);
    Run("ProfileAndSettings_CurrentSchemasRoundTrip", TestProfileAndSettingsSchemas);
    Run("Save_ExplicitMigrationChainIsTransactional", TestExplicitSaveMigrationChain);
    Run("Assets_SavedIdentityRegisters", TestSavedAssetIdentityRegistration);
    Run("Save_AwaitedWorkValidationIsShared", TestAwaitedWorkValidation);
    Run("Persistence_KeyUpgradeAndFailurePreservation",
        TestPersistenceKeyUpgradeAndFailurePreservation);
    Run("Persistence_PackagedUserDataMigration",
        TestPackagedUserDataMigration);
    Run("Persistence_OptionalPackPreferenceAtomicWrite",
        TestOptionalPackPreferenceAtomicWrite);
    if (g_failures == 0) std::cout << "PASS: runtime-save-load integration\n";
    return g_failures == 0 ? 0 : 1;
}

