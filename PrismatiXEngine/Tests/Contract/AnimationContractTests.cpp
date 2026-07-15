#include "Engine/UI/Animation.h"
#include "Engine/UI/Control.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Tests/TestSupport/TestHarness.h"

#include <cmath>

int main() {
    px::test::Suite suite("AnimationContract");
    suite.Expect(static_cast<bool>(px::ui::RegisterBuiltinUITypes()),
                 "animation contracts register authoritative UI properties");

    suite.Run("LibraryRoundTrip_RejectsBrokenReferences", [&] {
        const auto target = px::Uuid::FromName("PrismatiX.Animation.Target");
        px::ui::AnimationClip clip;
        const auto clipId = px::Uuid::FromName("PrismatiX.Animation.Clip");
        clip.id = clipId;
        clip.name = "Fade";
        clip.duration = 1.0f;
        clip.tracks.push_back(
            {.node = target,
             .property = "opacity",
             .keys = {{0.0f, 1.0, px::ui::Ease::Linear,
                       px::ui::KeyInterpolation::Linear},
                      {1.0f, 0.0, px::ui::Ease::Linear,
                       px::ui::KeyInterpolation::Linear}}});
        px::ui::UIAnimationLibrary library;
        const auto state = px::Uuid::FromName("PrismatiX.Animation.State");
        library.clips.push_back(std::move(clip));
        library.machine.entry = state;
        library.machine.states.push_back({state, "Fade", clipId, {0, 0}});
        const px::Variant encoded = px::ui::WriteUIAnimationLibrary(library);
        suite.Expect(static_cast<bool>(px::ui::ParseUIAnimationLibrary(
                         encoded, "animation-contract")),
                     "valid clip/state library round-trips");
        auto broken = encoded.Clone();
        auto* states = (*(*broken.AsObject())["machine"].AsObject())["states"].AsArray();
        (*states->front().AsObject())["clip"] =
            px::Uuid::FromName("PrismatiX.Animation.MissingClip");
        suite.Expect(!px::ui::ParseUIAnimationLibrary(broken, "missing-clip"),
                     "state reference to missing clip is rejected");
    });

    suite.Run("PlayerSeekStop_RestoresAuthoredDesignState", [&] {
        px::ui::Control control("Animated");
        px::ui::AnimationClip clip;
        clip.id = px::Uuid::FromName("PrismatiX.Animation.PlayerClip");
        clip.name = "Fade";
        clip.duration = 1.0f;
        clip.tracks.push_back(
            {.node = control.Id(),
             .property = "opacity",
             .keys = {{0.0f, 1.0, px::ui::Ease::Linear,
                       px::ui::KeyInterpolation::Linear},
                      {1.0f, 0.0, px::ui::Ease::Linear,
                       px::ui::KeyInterpolation::Linear}}});
        px::ui::AnimationPlayer player(control);
        suite.Expect(player.Play(clip) && player.Seek(0.5f) &&
                         std::abs(control.Opacity() - 0.5f) < 0.001f,
                     "runtime player interpolates the authored property at scrub time");
        suite.Expect(player.Stop(true) &&
                         std::abs(control.Opacity() - 1.0f) < 0.001f,
                     "stopping preview restores design state");
    });

    suite.Run("StateMachineTrigger_TransitionsAndCapturesDeterministically", [&] {
        auto root = std::make_unique<px::ui::Control>("Machine");
        const auto target = root->Id();
        const auto idleClip = px::Uuid::FromName("PrismatiX.Animation.IdleClip");
        const auto activeClip = px::Uuid::FromName("PrismatiX.Animation.ActiveClip");
        const auto makeClip = [&](px::Uuid id, std::string name, double opacity) {
            px::ui::AnimationClip clip;
            clip.id = id;
            clip.name = std::move(name);
            clip.duration = 1.0f;
            clip.tracks.push_back(
                {.node = target,
                 .property = "opacity",
                 .keys = {{0.0f, opacity, px::ui::Ease::Linear,
                           px::ui::KeyInterpolation::Linear}}});
            return clip;
        };
        const auto idle = px::Uuid::FromName("PrismatiX.Animation.Idle");
        const auto active = px::Uuid::FromName("PrismatiX.Animation.Active");
        px::ui::UIAnimationLibrary library;
        library.clips.push_back(makeClip(idleClip, "Idle", 1.0));
        library.clips.push_back(makeClip(activeClip, "Active", 0.25));
        library.machine.entry = idle;
        library.machine.states = {{idle, "Idle", idleClip, {0, 0}},
                                  {active, "Active", activeClip, {100, 0}}};
        library.machine.parameters = {
            {"go", px::ui::AnimationParameterType::Trigger, false}};
        library.machine.transitions.push_back(
            {px::Uuid::FromName("PrismatiX.Animation.Transition"), idle, active,
             {{"go", px::ui::AnimationConditionOperator::Triggered, false}},
             false, 1, 0, 0});
        px::ui::UIAnimationController controller(*root);
        suite.Expect(controller.SetLibrary(std::move(library)) &&
                         controller.SetTrigger("go") && controller.Update(0) &&
                         controller.CaptureState().state == active,
                     "trigger transitions from deterministic entry to target state");
        const auto checkpoint = controller.CaptureState();
        suite.Expect(controller.Travel(idle) && controller.RestoreState(checkpoint) &&
                         controller.CaptureState().state == active,
                     "state machine checkpoint restores exact state and parameters");
    });

    return suite.Finish();
}
