#include "Engine/UI/Animation.h"
#include "Engine/UI/Startup/SplashSequencePlayer.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Tests/TestSupport/TestHarness.h"

#include <unordered_map>

namespace {

using px::ui::startup::SplashScreenEntry;

px::resource::TypedDocument Scene(const std::string& name, const bool animations = true) {
    px::resource::TypedDocument document;
    document.kind = px::resource::DocumentKind::Scene;
    document.id = px::Uuid::FromName("PrismatiX.Splash.Test.Scene." + name);
    document.type = "UIScene";
    document.properties["canvasSize"] = px::Vec2{1280, 720};
    document.properties["uiSchemaVersion"] = std::int64_t{5};
    const px::Uuid root = px::Uuid::FromName("PrismatiX.Splash.Test.Root." + name);
    const px::Uuid logo = px::Uuid::FromName("PrismatiX.Splash.Test.Logo." + name);
    document.nodes.push_back(
        {root, {}, "Root", "Control", {{"anchors", px::Rect{0, 0, 1, 1}}}});
    document.nodes.push_back({logo,
                              root,
                              "Logo",
                              "Control",
                              {{"anchors", px::Rect{0.4f, 0.4f, 0.6f, 0.6f}},
                               {"opacity", 0.0}}});
    if (!animations) return document;

    const px::Uuid enterClip = px::Uuid::FromName("PrismatiX.Splash.EnterClip." + name);
    const px::Uuid exitClip = px::Uuid::FromName("PrismatiX.Splash.ExitClip." + name);
    const px::Uuid enterState = px::Uuid::FromName("PrismatiX.Splash.EnterState." + name);
    const px::Uuid exitState = px::Uuid::FromName("PrismatiX.Splash.ExitState." + name);
    px::ui::AnimationClip enter;
    enter.id = enterClip;
    enter.name = "enter";
    enter.duration = 0.5f;
    enter.tracks.push_back(
        {logo, "opacity", {{0.0f, 0.0}, {0.5f, 1.0}}});
    px::ui::AnimationClip exit;
    exit.id = exitClip;
    exit.name = "exit";
    exit.duration = 0.5f;
    exit.tracks.push_back(
        {logo, "opacity", {{0.0f, 1.0}, {0.5f, 0.0}}});
    px::ui::UIAnimationLibrary library;
    library.clips.push_back(std::move(enter));
    library.clips.push_back(std::move(exit));
    library.machine.entry = enterState;
    library.machine.states = {{enterState, "enter", enterClip, {0, 0}},
                              {exitState, "exit", exitClip, {220, 0}}};
    document.properties["animations"] = px::ui::WriteUIAnimationLibrary(library);
    return document;
}

SplashScreenEntry Entry(const std::string& name = "one") {
    SplashScreenEntry entry;
    entry.scene = {px::Uuid::FromName("PrismatiX.Splash.Asset." + name),
                   "Content/UI/Splash/" + name + ".pxscene"};
    entry.audio = px::ResourceRefValue{
        px::Uuid::FromName("PrismatiX.Splash.Audio." + name),
        "Content/Audio/SFX/Splash/" + name + ".wav"};
    return entry;
}

struct Fixture {
    std::unordered_map<std::string, px::resource::TypedDocument> scenes;
    std::vector<std::string> loaded;
    std::vector<std::string> audio;
    std::vector<px::diag::Diagnostic> diagnostics;

    px::ui::startup::SplashSequencePlayer Player() {
        return px::ui::startup::SplashSequencePlayer({
            .loadScene = [this](const px::ResourceRefValue& reference) {
                loaded.push_back(reference.lastKnownPath);
                const auto found = scenes.find(reference.lastKnownPath);
                if (found == scenes.end())
                    return px::Result<px::resource::TypedDocument>::Failure(
                        px::diag::Diagnostic{.severity = px::diag::Severity::Error,
                                             .code = "PXTEST",
                                             .category = "Player.Splash",
                                             .message = "missing scene"});
                return px::Result<px::resource::TypedDocument>::Success(found->second);
            },
            .playAudio = [this](const px::ResourceRefValue& reference) {
                audio.push_back(reference.lastKnownPath);
                return px::Status::Ok();
            },
            .diagnostics = [this](const px::diag::Diagnostic& diagnostic) {
                diagnostics.push_back(diagnostic);
            },
        });
    }
};

}  // namespace

int main() {
    px::test::Suite suite("SplashSequencePlayer");
    suite.Require(static_cast<bool>(px::ui::RegisterBuiltinUITypes()),
                  "runtime UI types register for splash tests");

    suite.Run("EmptySequence_CompletesImmediatelyExactlyOnce", [&] {
        Fixture fixture;
        auto player = fixture.Player();
        int completions = 0;
        player.SetCompletionCallback([&] { ++completions; });
        suite.Expect(static_cast<bool>(player.Start({})) && player.Completed() &&
                         completions == 1,
                     "empty splash sequence completes immediately");
        player.Update(20.0f, true);
        suite.Expect(completions == 1, "completion callback is emitted exactly once");
    });

    suite.Run("OneSplash_EnterHoldExitAudioAndComplete", [&] {
        Fixture fixture;
        const auto entry = Entry();
        fixture.scenes.emplace(entry.scene.lastKnownPath, Scene("one"));
        auto player = fixture.Player();
        int completions = 0;
        player.SetCompletionCallback([&] { ++completions; });
        suite.Require(static_cast<bool>(player.Start({entry})), "one splash starts");
        suite.Expect(player.CurrentPhase() == decltype(player)::Phase::Entering &&
                         fixture.audio.size() == 1 &&
                         player.Context().CaptureAnimationState().state ==
                             px::Uuid::FromName("PrismatiX.Splash.EnterState.one"),
                     "entry loads once, plays SE once, and starts enter animation");
        player.Update(0.5f);
        suite.Expect(player.CurrentPhase() == decltype(player)::Phase::Holding,
                     "enter transitions to hold");
        player.Update(1.0f);
        suite.Expect(player.CurrentPhase() == decltype(player)::Phase::Exiting &&
                         player.Context().CaptureAnimationState().state ==
                             px::Uuid::FromName("PrismatiX.Splash.ExitState.one"),
                     "minimum-duration planning starts configured exit animation");
        player.Update(0.5f);
        suite.Expect(player.Completed() && completions == 1 && fixture.loaded.size() == 1 &&
                         fixture.audio.size() == 1,
                     "one splash completes without per-frame reload or repeated audio");
    });

    suite.Run("MultipleSplashes_PreserveExactAuthoredOrder", [&] {
        Fixture fixture;
        std::vector<SplashScreenEntry> entries{Entry("publisher"), Entry("studio"),
                                               Entry("engine")};
        for (const auto& entry : entries)
            fixture.scenes.emplace(entry.scene.lastKnownPath,
                                   Scene(entry.scene.lastKnownPath));
        auto player = fixture.Player();
        player.Start(entries, true);
        player.Update(2.0f);
        player.Update(2.0f);
        player.Update(2.0f);
        suite.Expect(player.Completed() && fixture.loaded.size() == 3 &&
                         fixture.loaded[0] == entries[0].scene.lastKnownPath &&
                         fixture.loaded[1] == entries[1].scene.lastKnownPath &&
                         fixture.loaded[2] == entries[2].scene.lastKnownPath,
                     "array order is the only splash playback order");
    });

    suite.Run("SkipContract_RespectsEnabledAndThreshold", [&] {
        Fixture fixture;
        auto disabled = Entry("disabled");
        disabled.skippable = false;
        fixture.scenes.emplace(disabled.scene.lastKnownPath, Scene("disabled"));
        auto player = fixture.Player();
        player.Start({disabled});
        player.Update(0.6f, true);
        suite.Expect(player.CurrentPhase() != decltype(player)::Phase::Exiting,
                     "disabled skip input is ignored");

        auto enabled = Entry("enabled");
        fixture.scenes.emplace(enabled.scene.lastKnownPath, Scene("enabled"));
        player.Start({enabled});
        player.Update(0.25f, true);
        suite.Expect(player.CurrentPhase() == decltype(player)::Phase::Entering,
                     "skip before skipAllowedAfter is ignored");
        player.Update(0.25f);
        suite.Expect(player.CurrentPhase() == decltype(player)::Phase::Exiting,
                     "pending skip exits smoothly once threshold is reached");
    });

    suite.Run("MissingSceneAndAudio_AreNonFatalAtRuntime", [&] {
        Fixture fixture;
        auto missing = Entry("missing");
        auto valid = Entry("valid");
        fixture.scenes.emplace(valid.scene.lastKnownPath, Scene("valid"));
        auto player = px::ui::startup::SplashSequencePlayer({
            .loadScene = [&fixture](const px::ResourceRefValue& reference) {
                fixture.loaded.push_back(reference.lastKnownPath);
                const auto found = fixture.scenes.find(reference.lastKnownPath);
                if (found == fixture.scenes.end())
                    return px::Result<px::resource::TypedDocument>::Failure(
                        px::diag::Diagnostic{.severity = px::diag::Severity::Error,
                                             .code = "PXTEST",
                                             .category = "Player.Splash",
                                             .message = "missing scene"});
                return px::Result<px::resource::TypedDocument>::Success(found->second);
            },
            .playAudio = [](const px::ResourceRefValue&) {
                return px::Status::Fail(px::diag::Diagnostic{
                    .severity = px::diag::Severity::Warning,
                    .code = "PXBOOT1110",
                    .category = "Player.Splash",
                    .message = "missing optional audio"});
            },
            .diagnostics = [&fixture](const px::diag::Diagnostic& diagnostic) {
                fixture.diagnostics.push_back(diagnostic);
            },
        });
        player.Start({missing, valid}, true);
        suite.Expect(player.CurrentIndex() == 1 && !player.Completed() &&
                         fixture.diagnostics.size() >= 3,
                     "missing scene is diagnosed and skipped; missing audio keeps visuals");
        player.Update(2.0f);
        suite.Expect(player.Completed(), "valid visual splash completes after audio warning");
    });

    suite.Run("ReducedMotion_DisablesFadesButKeepsReadableDuration", [&] {
        Fixture fixture;
        const auto entry = Entry("reduced");
        fixture.scenes.emplace(entry.scene.lastKnownPath, Scene("reduced"));
        auto player = fixture.Player();
        player.Start({entry}, true);
        suite.Expect(player.CurrentPhase() == decltype(player)::Phase::Holding,
                     "reduced motion bypasses enter animation");
        player.Update(1.9f);
        suite.Expect(!player.Completed(), "minimum readable display remains enforced");
        player.Update(0.1f);
        suite.Expect(player.Completed(), "reduced-motion splash completes at minimum duration");
    });

    return suite.Finish();
}
