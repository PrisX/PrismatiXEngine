#include "Engine/UI/Animation.h"
#include "Engine/SDK/Motion.h"
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

    suite.Run("MotionClip_ValidatesHierarchyAndSamplesLayeredPose", [&] {
        constexpr auto source = R"({
          "format":"PrismatiXMotion","schemaRevision":1,"id":"logo",
          "canvas":{"width":1920,"height":1080},"duration":1,"loop":false,
          "layers":[
            {"id":"body","image":"Assets/body.png","width":100,"height":100,
             "x":100,"z":1},
            {"id":"mark","parent":"body","image":"Assets/mark.png",
             "width":20,"height":20,"x":20,"z":2,"visible":false}
          ],
          "tracks":[
            {"layer":"body","property":"opacity","keys":[
              {"time":0,"value":0,"curve":"linear"},
              {"time":1,"value":1,"curve":"linear"}]},
            {"layer":"mark","property":"visible","keys":[
              {"time":0.25,"value":true,"curve":"step"}]}
          ]})";
        auto parsed = px::sdk::ParseMotion(source, "logo.pxmotion");
        suite.Expect(parsed.Valid(), "valid motion document parses");
        if (!parsed.Valid()) return;
        const auto dependencies = px::sdk::MotionDependencies(*parsed.clip);
        suite.Expect(dependencies.size() == 2 &&
                         dependencies[0] == "Assets/body.png" &&
                         dependencies[1] == "Assets/mark.png",
                     "motion image dependencies are collected once");
        px::sdk::MotionSampler sampler(*parsed.clip);
        sampler.Seek(0.1);
        suite.Expect(!sampler.Pose()[1].visible,
                     "visibility steps without interpolation");
        sampler.Seek(0.5);
        suite.Expect(std::abs(sampler.Pose()[0].opacity - 0.5) < 0.0001 &&
                         std::abs(sampler.Pose()[1].opacity - 0.5) < 0.0001 &&
                         std::abs(sampler.Pose()[1].transform.tx - 120) < 0.0001 &&
                         sampler.Pose()[1].visible,
                     "seek composes child transforms and inherited alpha");
        sampler.Seek(0.5);
        suite.Expect(std::abs(sampler.Pose()[1].transform.tx - 120) < 0.0001,
                     "repeated seek is deterministic");
        const auto* retainedPose = sampler.Pose().data();
        for (int frame = 0; frame < 1000; ++frame)
            sampler.Seek(static_cast<double>(frame) / 1000);
        suite.Expect(sampler.Pose().data() == retainedPose,
                     "repeated seek reuses pose storage");
        auto cycle = std::string(source);
        cycle.replace(cycle.find("\"x\":100,\"z\":1"),
                      std::string("\"x\":100,\"z\":1").size(),
                      "\"x\":100,\"z\":1,\"parent\":\"mark\"");
        suite.Expect(!px::sdk::ParseMotion(cycle).Valid(),
                     "motion parent cycles fail closed");
        suite.Expect(!px::sdk::ParseMotion(
            R"({"format":7,"schemaRevision":1,"id":"bad"})").Valid() &&
                         !px::sdk::ParseMotion(
            R"({"format":"PrismatiXMotion","schemaRevision":18446744073709551615,"id":"bad"})").Valid(),
                     "malformed motion format and huge revisions report diagnostics");
    });

    return suite.Finish();
}
