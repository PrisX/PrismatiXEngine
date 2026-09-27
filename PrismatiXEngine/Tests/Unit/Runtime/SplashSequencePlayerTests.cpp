#include "Engine/SDK/Ui.h"
#include "Engine/UI/Startup/SplashSequencePlayer.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Engine/UI/UiApplication.h"
#include "Engine/UI/Widgets.h"
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
    px::ui::startup::SplashSequencePlayer::VisualPreloader visualPreloader;

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
            .preloadVisuals = visualPreloader,
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

    suite.Run("VisualPreload_KeepsPreviousSceneUntilImagesAreReady", [&] {
        Fixture fixture;
        const auto first = Entry("first");
        const auto second = Entry("second");
        fixture.scenes.emplace(first.scene.lastKnownPath, Scene("first", false));
        auto next = Scene("second", false);
        next.nodes[1].runtimeType = "TextureRect";
        next.nodes[1].kind = px::sdk::UiNodeKind::Image;
        next.nodes[1].runtimeProperties.emplace("path", std::string{"Assets/Splash/second.png"});
        fixture.scenes.emplace(second.scene.lastKnownPath, std::move(next));
        bool ready = false;
        std::vector<std::string> requested;
        fixture.visualPreloader = [&](const std::vector<std::string>& paths) {
            requested.insert(requested.end(), paths.begin(), paths.end());
            return [&ready, paths] { return paths.empty() || ready; };
        };
        auto player = fixture.Player();
        auto shortFirst = first;
        shortFirst.minimumDuration = 0.1f;
        suite.Require(static_cast<bool>(player.Start({shortFirst, second}, true)),
                      "visual preloading sequence starts");
        const auto* previousRoot = player.Context().Root();
        suite.Expect(previousRoot && player.PresentationGeneration() == 1,
                     "first presentation is installed");
        player.Update(0.1f);
        suite.Expect(player.CurrentPhase() == decltype(player)::Phase::Holding &&
                         player.Context().Root() == previousRoot &&
                         player.PresentationGeneration() == 1 &&
                         fixture.audio.size() == 1 &&
                         requested == std::vector<std::string>{"Assets/Splash/second.png"},
                     "pending texture keeps outgoing scene and delays next audio");
        const float waitingElapsed = player.Elapsed();
        player.Update(1.0f);
        suite.Expect(player.Elapsed() > waitingElapsed && player.Context().Root() == previousRoot,
                     "preload wait extends current scene instead of exposing an incomplete next scene");
        ready = true;
        player.Update(0.0f);
        suite.Expect(player.PresentationGeneration() == 2 &&
                         player.Context().Root() != previousRoot &&
                         fixture.audio.size() == 2 && fixture.loaded.size() == 2,
                     "new scene installs once only after texture batch completes");
    });

    suite.Run("MotionDependencies_ArePreloadedThroughAuthoredUi", [&] {
        auto motion = Scene("motion", false);
        motion.nodes[1].runtimeType = "MotionRect";
        motion.nodes[1].kind = px::sdk::UiNodeKind::Image;
        motion.nodes[1].runtimeProperties.emplace("path", std::string{"Assets/Motion/logo.pxmotion"});
        constexpr auto descriptor = R"({
          "format":"PrismatiXMotion","schemaRevision":1,"id":"logo",
          "canvas":{"width":320,"height":120},"duration":1,"loop":false,
          "layers":[{"id":"logo","image":"Assets/Motion/logo.png",
                     "width":320,"height":120}],"tracks":[]})";
        const auto dependencies = px::ui::CollectUiImageDependencies(
            motion, {}, {}, [](const std::string_view path) -> std::optional<std::string> {
                return path == "Assets/Motion/logo.pxmotion"
                    ? std::optional<std::string>{descriptor} : std::nullopt;
            });
        suite.Expect(dependencies && dependencies.Value() ==
                         std::vector<std::string>{"Assets/Motion/logo.png"},
                     "motion descriptor expands to image layers before presentation");
    });

    suite.Run("PortraitDependencies_ArePreloadedThroughAuthoredUi", [&] {
        auto portrait = Scene("portrait", false);
        portrait.nodes[1].runtimeType = "TextureRect";
        portrait.nodes[1].kind = px::sdk::UiNodeKind::Image;
        portrait.nodes[1].runtimeProperties.emplace("path", std::string{"Assets/Hero.pxportrait"});
        constexpr auto descriptor = R"({"format":"PrismatiXLayeredPortrait",
          "schemaRevision":2,"width":320,"height":480,"layers":[
          {"path":"Assets/Hero/base.png","width":320,"height":480,"role":"base"},
          {"path":"Assets/Hero/face.png","x":40,"y":60,"width":80,"height":80,
           "role":"expression"}]})";
        const auto loader = [](std::string_view path) -> std::optional<std::string> {
            return path == "Assets/Hero.pxportrait"
                ? std::optional<std::string>{descriptor} : std::nullopt;
        };
        const auto dependencies = px::ui::CollectUiImageDependencies(
            portrait, {}, {}, loader);
        suite.Expect(dependencies && dependencies.Value() ==
                         std::vector<std::string>{"Assets/Hero/base.png", "Assets/Hero/face.png"},
                     "portrait descriptor expands to ordered layers before presentation");
        auto catalogPortrait = portrait;
        catalogPortrait.nodes[1].runtimeProperties.erase("path");
        catalogPortrait.nodes[1].assetId = "11111111-1111-4111-8111-111111111111";
        const auto catalogDependencies = px::ui::CollectUiImageDependencies(
            catalogPortrait,
            [](std::string_view) -> std::optional<std::string> {
                return std::string{"Assets/Hero.pxportrait"};
            }, {}, loader);
        suite.Expect(catalogDependencies && catalogDependencies.Value() ==
                         std::vector<std::string>{"Assets/Hero/base.png", "Assets/Hero/face.png"},
                     "catalog-referenced portrait also expands to image layers");
        const auto missing = px::ui::CollectUiImageDependencies(portrait);
        suite.Expect(!missing, "portrait without text loader fails before scene installation");
    });

    suite.Run("MotionMarkers_ReachSplashAudioService", [&] {
        auto scene = Scene("motion-markers", false);
        scene.nodes[1].runtimeType = "MotionRect";
        scene.nodes[1].kind = px::sdk::UiNodeKind::Image;
        scene.nodes[1].runtimeProperties.emplace("path", std::string{"Assets/Motion/logo.pxmotion"});
        constexpr auto descriptor = R"({
          "format":"PrismatiXMotion","schemaRevision":3,"id":"logo",
          "canvas":{"width":320,"height":120},"duration":1,"loop":false,
          "layers":[{"id":"logo","image":"Assets/Motion/logo.png",
                     "width":320,"height":120}],"tracks":[],
          "markers":[{"id":"pop","time":0.25,"audio":"Assets/Audio/pop.ogg","bus":"se"}]})";
        std::vector<std::string> played;
        px::ui::startup::SplashSequencePlayer player({
            .loadScene = [scene](const px::ResourceRefValue&) {
                return px::Result<px::sdk::UiDocument>::Success(scene);
            },
            .loadMotion = [](const std::string_view path) -> std::optional<std::string> {
                return path == "Assets/Motion/logo.pxmotion"
                    ? std::optional<std::string>{descriptor} : std::nullopt;
            },
            .playMotionMarker = [&](const px::sdk::MotionAudioMarker& marker) {
                played.push_back(marker.audio);
            },
        });
        suite.Require(static_cast<bool>(player.Start({Entry("motion-markers")})),
                      "splash with authored motion starts");
        auto* motion = dynamic_cast<px::ui::MotionRect*>(
            player.Context().Root()->Find(*px::Uuid::Parse(scene.nodes[1].id)));
        suite.Require(motion != nullptr, "splash installs a MotionRect");
        motion->Update(0.5f);
        suite.Expect(played == std::vector<std::string>{"Assets/Audio/pop.ogg"},
                     "splash motion marker is delivered to the audio service");
    });

    suite.Run("SpriteText_GlyphImagesArePreloadedAndValidated", [&] {
        auto scene = Scene("sprite-text", false);
        scene.nodes[1].runtimeType = "SpriteText";
        scene.nodes[1].kind = px::sdk::UiNodeKind::Image;
        scene.nodes[1].runtimeProperties.emplace("text", std::string{"1-"});
        scene.nodes[1].runtimeProperties.emplace("advance", 24.0);
        scene.nodes[1].runtimeProperties.emplace("glyphs", px::sdk::UiArrayValue{R"([
          {"character":"1","path":"Assets/Glyph/one.png","x":0,"y":2,"width":18,"height":30},
          {"character":"-","path":"Assets/Glyph/dash.png","x":1,"y":3,"width":19,"height":10}
        ])"});
        const auto dependencies = px::ui::CollectUiImageDependencies(scene);
        suite.Expect(dependencies && dependencies.Value() ==
                         std::vector<std::string>{"Assets/Glyph/one.png", "Assets/Glyph/dash.png"},
                     "one SpriteText node preloads every possible glyph image");
        px::ui::UIContext context;
        px::ui::UiApplication application(context);
        const auto applied = application.ApplyDocument(scene);
        suite.Require(static_cast<bool>(applied), "SpriteText applies through the shared UI path");
        auto* sprite = dynamic_cast<px::ui::SpriteText*>(
            context.Root()->Find(*px::Uuid::Parse(scene.nodes[1].id)));
        suite.Require(sprite != nullptr, "SpriteText creates a typed runtime control");
        suite.Expect(sprite->Text() == "1-" && sprite->Glyphs().size() == 2 &&
                         sprite->Advance() == 24,
                     "SpriteText glyph data and spacing reach the runtime");
        auto invalid = scene;
        invalid.nodes[1].runtimeProperties["glyphs"] = px::sdk::UiArrayValue{R"([
          {"character":"1","path":"../outside.png","x":0,"y":2,"width":18,"height":30}
        ])"};
        suite.Expect(!application.ApplyDocument(invalid),
                     "unsafe SpriteText glyph paths fail before replacing the live UI");
    });

    suite.Run("CompletionPreload_HoldsFinalSplashUntilTitleIsReady", [&] {
        auto entry = Entry("final");
        entry.minimumDuration = 0.1f;
        bool titleReady = false;
        int completed = 0;
        auto document = Scene("final", false);
        px::ui::startup::SplashSequencePlayer player({
            .loadScene = [document](const px::ResourceRefValue&) {
                return px::Result<px::sdk::UiDocument>::Success(document);
            },
            .completionVisualsReady = [&] { return titleReady; },
        });
        player.SetCompletionCallback([&] { ++completed; });
        suite.Require(static_cast<bool>(player.Start({entry}, true)),
                      "final splash starts with title preloader");
        const auto* finalRoot = player.Context().Root();
        player.Update(0.5f);
        suite.Expect(player.CurrentPhase() == decltype(player)::Phase::Holding &&
                         player.Context().Root() == finalRoot && completed == 0,
                     "final splash remains visible instead of exposing an unready title");
        titleReady = true;
        player.Update(0.0f);
        suite.Expect(player.Completed() && completed == 1,
                     "title readiness releases final splash exactly once");
    });

    suite.Run("DelayedAudio_FiresOnceAtConfiguredTime", [&] {
        Fixture fixture;
        auto entry = Entry("delayed");
        entry.audioDelay = 1.0f;
        fixture.scenes.emplace(entry.scene.lastKnownPath, Scene("delayed"));
        auto player = fixture.Player();
        suite.Require(static_cast<bool>(player.Start({entry})), "delayed splash starts");
        suite.Expect(fixture.audio.empty(), "audio does not play at scene load");
        player.Update(0.9f);
        suite.Expect(fixture.audio.empty(), "audio remains silent before delay");
        player.Update(0.1f);
        suite.Expect(fixture.audio.size() == 1 &&
                         fixture.audio[0] == entry.audio->lastKnownPath,
                     "audio plays at its authored delay");
        player.Update(1.0f);
        suite.Expect(fixture.audio.size() == 1,
                     "subsequent frames do not replay the audio");
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
