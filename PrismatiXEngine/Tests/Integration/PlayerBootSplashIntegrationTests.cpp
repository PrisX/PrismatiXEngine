#include "Engine/SDK/Ui.h"
#include "Engine/UI/Startup/SplashSequencePlayer.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Tests/TestSupport/TestHarness.h"

#include <unordered_map>

namespace {

px::sdk::UiDocument Scene(const std::string& name) {
    px::sdk::UiDocument document;
    document.id = px::Uuid::FromName("BootScene/" + name).ToString();
    document.revision = 1;
    document.name = name;
    document.width = 1280;
    document.height = 720;
    const auto root = px::Uuid::FromName("BootRoot/" + name).ToString();
    const auto logo = px::Uuid::FromName("BootLogo/" + name).ToString();
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
    document.nodes = {std::move(rootNode), std::move(logoNode)};
    px::sdk::UiAnimationClip enter;
    enter.id = px::Uuid::FromName("BootEnter/" + name).ToString();
    enter.name = "enter";
    enter.duration = 0.1f;
    px::sdk::UiAnimationClip exit;
    exit.id = px::Uuid::FromName("BootExit/" + name).ToString();
    exit.name = "exit";
    exit.duration = 0.1f;
    const auto enterState = px::Uuid::FromName("BootEnterState/" + name).ToString();
    const auto exitState = px::Uuid::FromName("BootExitState/" + name).ToString();
    px::sdk::UiAnimations animations;
    animations.clips.push_back(std::move(enter));
    animations.clips.push_back(std::move(exit));
    animations.stateMachine.entryStateId = enterState;
    animations.stateMachine.states = {
        {enterState,"enter",animations.clips[0].id,0,0},
        {exitState,"exit",animations.clips[1].id,220,0}};
    document.animations = std::move(animations);
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
                      return px::Result<px::sdk::UiDocument>::Failure(
                          px::diag::Diagnostic{.severity = px::diag::Severity::Error,
                                               .code = "PXTESTBOOT",
                                               .category = "Player.Splash",
                                               .message = "scene missing"});
                  return px::Result<px::sdk::UiDocument>::Success(found->second);
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

    std::unordered_map<std::string, px::sdk::UiDocument> scenes;
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
