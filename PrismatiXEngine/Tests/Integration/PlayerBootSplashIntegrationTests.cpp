#include "Engine/UI/Animation.h"
#include "Engine/UI/Startup/SplashSequencePlayer.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Tests/TestSupport/TestHarness.h"

#include <unordered_map>

namespace {

px::resource::TypedDocument Scene(const std::string& name) {
    px::resource::TypedDocument document;
    document.kind = px::resource::DocumentKind::Scene;
    document.id = px::Uuid::FromName("BootScene/" + name);
    document.type = "UIScene";
    document.properties["canvasSize"] = px::Vec2{1280, 720};
    document.properties["uiSchemaVersion"] = std::int64_t{5};
    const auto root = px::Uuid::FromName("BootRoot/" + name);
    const auto logo = px::Uuid::FromName("BootLogo/" + name);
    document.nodes.push_back({root, {}, "Root", "Control",
                              {{"anchors", px::Rect{0, 0, 1, 1}}}});
    document.nodes.push_back({logo, root, "Logo", "Control", {{"opacity", 1.0}}});
    px::ui::AnimationClip enter;
    enter.id = px::Uuid::FromName("BootEnter/" + name);
    enter.name = "enter";
    enter.duration = 0.1f;
    px::ui::AnimationClip exit;
    exit.id = px::Uuid::FromName("BootExit/" + name);
    exit.name = "exit";
    exit.duration = 0.1f;
    const auto enterState = px::Uuid::FromName("BootEnterState/" + name);
    const auto exitState = px::Uuid::FromName("BootExitState/" + name);
    px::ui::UIAnimationLibrary animations;
    animations.clips.push_back(std::move(enter));
    animations.clips.push_back(std::move(exit));
    animations.machine.entry = enterState;
    animations.machine.states = {
        {enterState, "enter", animations.clips[0].id, {0, 0}},
        {exitState, "exit", animations.clips[1].id, {220, 0}}};
    document.properties["animations"] = px::ui::WriteUIAnimationLibrary(animations);
    return document;
}

px::ui::startup::SplashScreenEntry Entry(const std::string& name) {
    px::ui::startup::SplashScreenEntry entry;
    entry.scene = {px::Uuid::FromName("BootAsset/" + name),
                   "Content/UI/Splash/" + name + ".pxscene"};
    entry.audio.reset();
    entry.minimumDuration = 0.2f;
    entry.skipAllowedAfter = 0.1f;
    return entry;
}

class HeadlessBootLifecycle {
public:
    HeadlessBootLifecycle()
        : player({
              .loadScene = [this](const px::ResourceRefValue& reference) {
                  loadOrder.push_back(reference.lastKnownPath);
                  const auto found = scenes.find(reference.lastKnownPath);
                  if (found == scenes.end())
                      return px::Result<px::resource::TypedDocument>::Failure(
                          px::diag::Diagnostic{.severity = px::diag::Severity::Error,
                                               .code = "PXTESTBOOT",
                                               .category = "Player.Splash",
                                               .message = "scene missing"});
                  return px::Result<px::resource::TypedDocument>::Success(found->second);
              },
          }) {
        player.SetCompletionCallback([this] { FinishBootPresentation(); });
    }

    void Start(std::vector<px::ui::startup::SplashScreenEntry> entries) {
        (void)player.Start(std::move(entries));
    }

    void Tick(const float delta) {
        if (!titleActive) player.Update(delta);
        else ++scenarioUpdates;
    }

    std::unordered_map<std::string, px::resource::TypedDocument> scenes;
    std::vector<std::string> loadOrder;
    px::ui::startup::SplashSequencePlayer player;
    bool routeReplaced = false;
    bool titleActive = false;
    int scenarioUpdates = 0;
    int completions = 0;

private:
    void FinishBootPresentation() {
        ++completions;
        routeReplaced = true;
        titleActive = true;
    }
};

}  // namespace

int main() {
    px::test::Suite suite("PlayerBootSplashIntegration");
    suite.Require(static_cast<bool>(px::ui::RegisterBuiltinUITypes()),
                  "runtime UI metadata registers for headless boot");

    suite.Run("NoSplashes_ActivatesTitleImmediately", [&] {
        HeadlessBootLifecycle boot;
        boot.Start({});
        suite.Expect(boot.titleActive && boot.routeReplaced && boot.completions == 1,
                     "empty boot sequence uses the single FinishBootPresentation boundary");
    });

    suite.Run("OneSplash_DefersTitleRouteAndScenario", [&] {
        HeadlessBootLifecycle boot;
        const auto entry = Entry("Engine");
        boot.scenes.emplace(entry.scene.lastKnownPath, Scene("Engine"));
        boot.Start({entry});
        suite.Expect(!boot.titleActive && !boot.routeReplaced && boot.scenarioUpdates == 0,
                     "title, route, and scenario remain inactive while splash is active");
        boot.Tick(0.1f);
        boot.Tick(0.1f);
        suite.Expect(boot.titleActive && boot.routeReplaced && boot.scenarioUpdates == 0 &&
                         boot.completions == 1,
                     "completion activates Title and route exactly once after splash");
    });

    suite.Run("ThreeSplashes_PlayExactOrderThenTitle", [&] {
        HeadlessBootLifecycle boot;
        std::vector entries{Entry("Publisher"), Entry("Studio"), Entry("Engine")};
        for (const auto& entry : entries)
            boot.scenes.emplace(entry.scene.lastKnownPath,
                                Scene(entry.scene.lastKnownPath));
        boot.Start(entries);
        for (int index = 0; index < 9 && !boot.titleActive; ++index) boot.Tick(0.1f);
        suite.Expect(boot.titleActive && boot.loadOrder.size() == 3 &&
                         boot.loadOrder[0] == entries[0].scene.lastKnownPath &&
                         boot.loadOrder[1] == entries[1].scene.lastKnownPath &&
                         boot.loadOrder[2] == entries[2].scene.lastKnownPath &&
                         boot.scenarioUpdates == 0,
                     "array order is preserved and scenario never updates during boot");
    });

    return suite.Finish();
}
