#include "Engine/UI/Animation.h"
#include "Engine/SDK/Motion.h"
#include "Engine/UI/Control.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Tests/TestSupport/TestHarness.h"

#include <cmath>
#include <nlohmann/json.hpp>

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

    suite.Run("MotionClip_LayerBlendModesAreVersionedAndValidated", [&] {
        constexpr auto source = R"({
          "format":"PrismatiXMotion","schemaRevision":4,"id":"blend",
          "canvas":{"width":320,"height":120},"duration":1,"loop":false,
          "layers":[
            {"id":"base","image":"Assets/base.png","width":100,"height":100},
            {"id":"light","image":"Assets/light.png","width":100,"height":100,
             "blendMode":"additive"},
            {"id":"shade","image":"Assets/shade.png","width":100,"height":100,
             "blendMode":"multiply"}],"tracks":[]})";
        const auto parsed = px::sdk::ParseMotion(source);
        suite.Expect(parsed.Valid() && parsed.clip->layers[0].blendMode ==
            px::sdk::MotionBlendMode::Normal && parsed.clip->layers[1].blendMode ==
            px::sdk::MotionBlendMode::Additive && parsed.clip->layers[2].blendMode ==
            px::sdk::MotionBlendMode::Multiply,
            "revision-4 blend modes parse without changing ordinary layers");
        auto oldRevision = std::string(source);
        oldRevision.replace(oldRevision.find("\"schemaRevision\":4"),
            std::string("\"schemaRevision\":4").size(), "\"schemaRevision\":3");
        suite.Expect(!px::sdk::ParseMotion(oldRevision).Valid(),
            "older motion revisions reject blendMode rather than ignoring it");
        auto invalid = std::string(source);
        invalid.replace(invalid.find("\"additive\""),
            std::string("\"additive\"").size(), "\"screen\"");
        suite.Expect(!px::sdk::ParseMotion(invalid).Valid(),
            "unsupported blend mode fails closed");
    });

    suite.Run("MotionClip_AlphaMasksAreVersionedAndValidated", [&] {
        constexpr auto source = R"({
          "format":"PrismatiXMotion","schemaRevision":6,"id":"masked",
          "canvas":{"width":320,"height":120},"duration":1,"loop":false,
          "layers":[
            {"id":"content","image":"Assets/content.png","width":100,
             "height":100,"mask":"coverage"},
            {"id":"coverage","image":"Assets/mask.png","width":100,
             "height":100,"x":10}],
          "tracks":[{"layer":"coverage","property":"x","keys":[
            {"time":0,"value":10,"curve":"linear"},
            {"time":1,"value":30,"curve":"linear"}]}]})";
        const auto parsed = px::sdk::ParseMotion(source);
        suite.Require(parsed.Valid(), "revision-6 mask reference parses");
        suite.Expect(parsed.clip->layers[0].mask == 1 &&
                     parsed.clip->maskSource[1] && !parsed.clip->maskSource[0],
                     "mask source is identified without adding a dependency kind");
        px::sdk::MotionSampler sampler(*parsed.clip);
        sampler.Seek(0.5);
        suite.Expect(std::abs(sampler.LayerPose(1).transform.tx - 20) < 0.001,
                     "mask layer follows the same deterministic clock");
        auto old = nlohmann::json::parse(source);
        old["schemaRevision"] = 5;
        suite.Expect(!px::sdk::ParseMotion(old.dump()).Valid(),
                     "older revisions reject mask instead of ignoring it");
        auto unknown = nlohmann::json::parse(source);
        unknown["layers"][0]["mask"] = "missing";
        suite.Expect(!px::sdk::ParseMotion(unknown.dump()).Valid(),
                     "unknown mask source fails closed");
        auto nested = nlohmann::json::parse(source);
        nested["layers"][1]["mask"] = "content";
        suite.Expect(!px::sdk::ParseMotion(nested.dump()).Valid(),
                     "nested mask cycle is rejected");
        auto unsupportedBlend = nlohmann::json::parse(source);
        unsupportedBlend["layers"][0]["blendMode"] = "multiply";
        suite.Expect(!px::sdk::ParseMotion(unsupportedBlend.dump()).Valid(),
                     "masked multiply blending is rejected until it is implemented");
    });

    suite.Run("MotionClip_GroupsRemapChildTimeWithoutDrawing", [&] {
        constexpr auto source = R"({
          "format":"PrismatiXMotion","schemaRevision":7,"id":"subtimeline",
          "canvas":{"width":320,"height":120},"duration":1,"loop":false,
          "layers":[
            {"id":"group","kind":"group","x":100,"y":30,
             "opacity":0.5,"timeOffset":-0.25,"timeScale":2},
            {"id":"image","parent":"group","image":"Assets/mark.png",
             "width":20,"height":20}],
          "tracks":[{"layer":"image","property":"x","keys":[
            {"time":0,"value":0,"curve":"linear"},
            {"time":1,"value":100,"curve":"linear"}]}]})";
        const auto parsed = px::sdk::ParseMotion(source);
        suite.Require(parsed.Valid(), "revision-7 group and local time parse");
        suite.Expect(parsed.clip->layers[0].kind == px::sdk::MotionLayerKind::Group &&
                     px::sdk::MotionDependencies(*parsed.clip) ==
                         std::vector<std::string>{"Assets/mark.png"},
                     "non-drawing groups do not become package resources");
        px::sdk::MotionSampler sampler(*parsed.clip);
        sampler.Seek(0.375);
        suite.Expect(std::abs(sampler.LayerPose(1).transform.tx - 150) < 0.001 &&
                     std::abs(sampler.LayerPose(1).opacity - 0.5) < 0.001,
                     "child uses its parent-local clock and inherited transform");
        auto old = nlohmann::json::parse(source);
        old["schemaRevision"] = 6;
        suite.Expect(!px::sdk::ParseMotion(old.dump()).Valid(),
                     "older revisions reject local clocks and groups");
        auto drawableGroup = nlohmann::json::parse(source);
        drawableGroup["layers"][0]["image"] = "Assets/invalid.png";
        suite.Expect(!px::sdk::ParseMotion(drawableGroup.dump()).Valid(),
                     "group layers cannot silently draw an image");
        auto maskedGroup = nlohmann::json::parse(source);
        maskedGroup["layers"][1]["mask"] = "group";
        suite.Expect(!px::sdk::ParseMotion(maskedGroup.dump()).Valid(),
                     "non-image groups cannot be mask sources");
    });

    suite.Run("MotionClip_CubicBezierTimingMatchesAuthoredControlPoints", [&] {
        constexpr auto source = R"({
          "format":"PrismatiXMotion","schemaRevision":5,"id":"eased",
          "canvas":{"width":320,"height":120},"duration":1,"loop":false,
          "layers":[{"id":"mark","image":"Assets/mark.png",
            "width":100,"height":100}],
          "tracks":[{"layer":"mark","property":"opacity","keys":[
            {"time":0,"value":0,"curve":"cubicBezier",
             "bezier":{"x1":0.1,"y1":0.8,"x2":0.4,"y2":0.9}},
            {"time":1,"value":1,"curve":"linear"}]}]})";
        const auto parsed = px::sdk::ParseMotion(source);
        suite.Require(parsed.Valid(), "revision-5 cubic timing parses");
        px::sdk::MotionSampler sampler(*parsed.clip);
        sampler.Seek(0.5);
        const double halfway = sampler.Pose()[0].opacity;
        suite.Expect(halfway > 0.75 && halfway < 1.0,
                     "cubic timing changes the sampled pose, not only metadata");
        sampler.Seek(0.5);
        suite.Expect(std::abs(sampler.Pose()[0].opacity - halfway) < 0.0000001,
                     "cubic timing seek is deterministic");
        auto oldRevision = std::string(source);
        oldRevision.replace(oldRevision.find("\"schemaRevision\":5"),
            std::string("\"schemaRevision\":5").size(), "\"schemaRevision\":4");
        suite.Expect(!px::sdk::ParseMotion(oldRevision).Valid(),
                     "older motion revisions reject cubic timing");
        auto missingControls = nlohmann::json::parse(source);
        missingControls["tracks"][0]["keys"][0].erase("bezier");
        suite.Expect(!px::sdk::ParseMotion(missingControls.dump()).Valid(),
                     "cubic timing requires all control points");
        auto invalidControls = nlohmann::json::parse(source);
        invalidControls["tracks"][0]["keys"][0]["bezier"]["x1"] = 1.1;
        suite.Expect(!px::sdk::ParseMotion(invalidControls.dump()).Valid(),
                     "cubic timing rejects invalid time control points");
    });

    suite.Run("MotionClip_AtlasFrameTrackIsDiscreteAndVersioned", [&] {
        constexpr auto source = R"({
          "format":"PrismatiXMotion","schemaRevision":2,"id":"atlas",
          "canvas":{"width":320,"height":120},"duration":1,"loop":false,
          "layers":[{"id":"logo","image":"Assets/logo-atlas.png",
            "width":120,"height":40,"frames":[
              {"x":0,"y":0,"width":120,"height":40},
              {"x":120,"y":0,"width":120,"height":40}]}],
          "tracks":[{"layer":"logo","property":"frame","keys":[
            {"time":0,"value":0,"curve":"step"},
            {"time":0.5,"value":1,"curve":"step"}]}]})";
        auto parsed = px::sdk::ParseMotion(source);
        suite.Expect(parsed.Valid(), "revision-2 atlas motion parses");
        if (!parsed.Valid()) return;
        px::sdk::MotionSampler sampler(*parsed.clip);
        sampler.Seek(0.25);
        suite.Expect(sampler.Pose()[0].sourcePixels &&
                         sampler.Pose()[0].sourcePixels->x == 0,
                     "first atlas frame remains selected before the step");
        sampler.Seek(0.75);
        suite.Expect(sampler.Pose()[0].sourcePixels &&
                         sampler.Pose()[0].sourcePixels->x == 120,
                     "second atlas frame is selected after the step");
        auto oldRevision = std::string(source);
        oldRevision.replace(oldRevision.find("\"schemaRevision\":2"),
            std::string("\"schemaRevision\":2").size(), "\"schemaRevision\":1");
        suite.Expect(!px::sdk::ParseMotion(oldRevision).Valid(),
                     "revision 1 does not silently reinterpret atlas fields");
        auto invalidFrame = std::string(source);
        invalidFrame.replace(invalidFrame.find("\"value\":1"),
            std::string("\"value\":1").size(), "\"value\":2");
        suite.Expect(!px::sdk::ParseMotion(invalidFrame).Valid(),
                     "out-of-range atlas frame is rejected");
        auto fractionalFrame = std::string(source);
        fractionalFrame.replace(fractionalFrame.find("\"value\":1"),
            std::string("\"value\":1").size(), "\"value\":0.5");
        suite.Expect(!px::sdk::ParseMotion(fractionalFrame).Valid(),
                     "fractional atlas frame is rejected");
        auto interpolatedFrame = std::string(source);
        interpolatedFrame.replace(interpolatedFrame.find("\"value\":1,\"curve\":\"step\""),
            std::string("\"value\":1,\"curve\":\"step\"").size(),
            "\"value\":1,\"curve\":\"linear\"");
        suite.Expect(!px::sdk::ParseMotion(interpolatedFrame).Valid(),
                     "atlas frames cannot interpolate between source rectangles");
        auto integralDecimal = std::string(source);
        integralDecimal.replace(integralDecimal.find("\"value\":1"),
            std::string("\"value\":1").size(), "\"value\":1.0");
        suite.Expect(px::sdk::ParseMotion(integralDecimal).Valid(),
                     "native accepts integral numeric JSON like authoring validation");
    });

    suite.Run("MotionClip_AudioMarkersAreVersionedAndPackaged", [&] {
        constexpr auto source = R"({
          "format":"PrismatiXMotion","schemaRevision":3,"id":"logo",
          "canvas":{"width":320,"height":120},"duration":1,"loop":false,
          "layers":[{"id":"body","image":"Assets/body.png","width":100,"height":100}],
          "tracks":[],"markers":[
            {"id":"start","time":0,"audio":"Assets/Audio/start.ogg","bus":"se"},
            {"id":"voice","time":0.5,"audio":"Assets/Audio/voice.ogg","bus":"voice"}]})";
        const auto parsed = px::sdk::ParseMotion(source);
        suite.Require(parsed.Valid(), "revision-3 motion audio markers parse");
        suite.Expect(parsed.clip->markers.size() == 2 &&
                         parsed.clip->markers[1].bus == px::sdk::MotionAudioBus::Voice,
                     "marker audio bus is typed");
        const auto sounds = px::sdk::MotionAudioDependencies(*parsed.clip);
        suite.Expect(sounds == std::vector<std::string>{
                         "Assets/Audio/start.ogg", "Assets/Audio/voice.ogg"},
                     "marker audio dependencies are separate from preloaded images");
        auto old = std::string(source);
        old.replace(old.find("\"schemaRevision\":3"),
                    std::string("\"schemaRevision\":3").size(),
                    "\"schemaRevision\":2");
        suite.Expect(!px::sdk::ParseMotion(old).Valid(),
                     "older motion revisions reject markers");
        auto duplicate = std::string(source);
        duplicate.replace(duplicate.find("\"id\":\"voice\""),
                          std::string("\"id\":\"voice\"").size(),
                          "\"id\":\"start\"");
        suite.Expect(!px::sdk::ParseMotion(duplicate).Valid(),
                     "duplicate marker identities fail closed");
    });

    return suite.Finish();
}
