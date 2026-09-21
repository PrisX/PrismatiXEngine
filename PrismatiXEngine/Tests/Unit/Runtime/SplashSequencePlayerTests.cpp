#include "Engine/SDK/Ui.h"
#include "Engine/UI/Startup/SplashSequencePlayer.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Tests/TestSupport/TestHarness.h"

#include <unordered_map>

namespace {

using px::ui::startup::SplashScreenEntry;

px::sdk::UiDocument Scene(const std::string& name, const bool animations = true) {
    px::sdk::UiDocument document;
    document.id = px::Uuid::FromName("PrismatiX.Splash.Test.Scene." + name).ToString();
    document.revision = 1;
    document.name = name;
    document.width = 1280;
    document.height = 720;
    const std::string root = px::Uuid::FromName("PrismatiX.Splash.Test.Root." + name).ToString();
    const std::string logo = px::Uuid::FromName("PrismatiX.Splash.Test.Logo." + name).ToString();
    document.rootId = root;
    px::sdk::UiNode rootNode;
    rootNode.id = root;
    rootNode.name = "Root";
    rootNode.kind = px::sdk::UiNodeKind::Group;
    rootNode.runtimeType = "Panel";
    rootNode.layout = {.width=1280,.height=720,.anchorRight=1,.anchorBottom=1,
                       .alignment="fill",.sizeRule="fill"};
    px::sdk::UiNode logoNode;
    logoNode.id = logo;
    logoNode.parentId = root;
    logoNode.name = "Logo";
    logoNode.runtimeType = "Control";
    logoNode.layout = {.x=512,.y=288,.width=256,.height=144,
                       .alignment="start",.sizeRule="fixed"};
    logoNode.appearance.opacity = 0.0f;
    document.nodes = {std::move(rootNode), std::move(logoNode)};
    if (!animations) return document;

    const std::string enterClip = px::Uuid::FromName("PrismatiX.Splash.EnterClip." + name).ToString();
    const std::string exitClip = px::Uuid::FromName("PrismatiX.Splash.ExitClip." + name).ToString();
    const std::string enterState = px::Uuid::FromName("PrismatiX.Splash.EnterState." + name).ToString();
    const std::string exitState = px::Uuid::FromName("PrismatiX.Splash.ExitState." + name).ToString();
    px::sdk::UiAnimationClip enter;
    enter.id = enterClip;
    enter.name = "enter";
    enter.duration = 0.5f;
    enter.tracks.push_back({px::Uuid::FromName("enter-track." + name).ToString(), logo, "opacity",
                            {{px::Uuid::FromName("enter-key-0." + name).ToString(),0.0f,0.0},
                             {px::Uuid::FromName("enter-key-1." + name).ToString(),0.5f,1.0}}});
    px::sdk::UiAnimationClip exit;
    exit.id = exitClip;
    exit.name = "exit";
    exit.duration = 0.5f;
    exit.tracks.push_back({px::Uuid::FromName("exit-track." + name).ToString(), logo, "opacity",
                           {{px::Uuid::FromName("exit-key-0." + name).ToString(),0.0f,1.0},
                            {px::Uuid::FromName("exit-key-1." + name).ToString(),0.5f,0.0}}});
    px::sdk::UiAnimations library;
    library.clips = {std::move(enter), std::move(exit)};
    library.stateMachine.entryStateId = enterState;
    library.stateMachine.states = {{enterState,"enter",enterClip,0,0},
                                   {exitState,"exit",exitClip,220,0}};
    document.animations = std::move(library);
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
    std::unordered_map<std::string, px::sdk::UiDocument> scenes;
    std::vector<std::string> loaded;
    std::vector<std::string> audio;
    std::vector<px::diag::Diagnostic> diagnostics;

    px::ui::startup::SplashSequencePlayer Player() {
        return px::ui::startup::SplashSequencePlayer({
            .loadScene = [this](const px::ResourceRefValue& reference) {
                loaded.push_back(reference.lastKnownPath);
                const auto found = scenes.find(reference.lastKnownPath);
                if (found == scenes.end())
                    return px::Result<px::sdk::UiDocument>::Failure(
                        px::diag::Diagnostic{.severity = px::diag::Severity::Error,
                                             .code = "PXTEST",
                                             .category = "Player.Splash",
                                             .message = "missing scene"});
                return px::Result<px::sdk::UiDocument>::Success(found->second);
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
                    return px::Result<px::sdk::UiDocument>::Failure(
                        px::diag::Diagnostic{.severity = px::diag::Severity::Error,
                                             .code = "PXTEST",
                                             .category = "Player.Splash",
                                             .message = "missing scene"});
                return px::Result<px::sdk::UiDocument>::Success(found->second);
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
