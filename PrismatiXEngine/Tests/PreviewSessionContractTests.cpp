#include "Engine/Audio/AudioEngine.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/IO/VFS.h"
#include "Engine/Preview/PreviewSessionFactory.h"
#include "Engine/Session/RuntimeSession.h"
#include "Engine/Platform/Input.h"
#include "Engine/UI/UIContext.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Tests/TestSupport/TestHarness.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <memory>
#include <string>

namespace {

using Json = nlohmann::json;

Json Operation(const std::string& id, const std::string& kind,
               Json arguments = Json::object()) {
    return {{"operationId", id},
            {"sourceId", id},
            {"sourceLine", 1},
            {"kind", kind},
            {"text", ""},
            {"arguments", std::move(arguments)}};
}

std::string Document(const std::string& id, const std::uint64_t revision,
                     Json operations) {
    return Json{{"format", "PrismatiXRuntimeIR"},
                {"schemaRevision", 2},
                {"documentId", id},
                {"committedRevision", revision},
                {"operations", std::move(operations)}}
        .dump();
}

struct RuntimeFixture {
    px::io::VFS vfs;
    px::audio::AudioEngine audio{vfs};
    px::graphics::AssetCache assets{nullptr, vfs};
    px::graphics::Renderer2D renderer{nullptr, assets};
    px::RuntimeSession runtime{{vfs, audio, renderer, assets}};
};

}  // namespace

int main() {
    px::test::Suite suite("PreviewSessionContract");

    suite.Run("FrontendNeutralSessionState", [&] {
        RuntimeFixture fixture;
        auto preview = px::preview::CreatePreviewSession(fixture.runtime);
        suite.Expect(preview->Tick(1, 0.0f).accepted,
                     "timeline and UI-only sessions tick before story apply");
        const auto checkpoint = preview->CaptureCheckpoint();
        suite.Require(checkpoint.accepted && checkpoint.checkpoint.has_value(),
                      "timeline and UI-only sessions can capture state");
        suite.Expect(
            preview->RestoreCheckpoint(checkpoint.checkpoint->id).accepted,
            "timeline and UI-only state restores without a VM program");
    });

    suite.Run("CompleteUIRuntimeCheckpoint", [&] {
        RuntimeFixture fixture;
        suite.Require(static_cast<bool>(px::ui::RegisterBuiltinUITypes()),
                      "built-in typed UI properties register for checkpoint testing");
        px::ui::UIContext ui;
        auto root = std::make_unique<px::ui::Control>("CheckpointRoot");
        const px::Uuid node = root->Id();
        suite.Require(static_cast<bool>(ui.SetRoot(std::move(root))),
                      "UI checkpoint root installs");
        px::ui::AnimationClip clip;
        clip.id = px::Uuid::FromName("preview.checkpoint.ui.clip");
        clip.name = "Checkpoint";
        clip.duration = 1.0f;
        clip.tracks.push_back({
            .node=node,.property="opacity",
            .keys={{0.0f,1.0,px::ui::Ease::Linear,
                    px::ui::KeyInterpolation::Linear},
                   {1.0f,0.0,px::ui::Ease::Linear,
                    px::ui::KeyInterpolation::Linear}}});
        px::ui::UIAnimationLibrary library;
        const px::Uuid clipId = clip.id;
        const px::Uuid animationState =
            px::Uuid::FromName("preview.checkpoint.ui.state");
        library.clips.push_back(std::move(clip));
        library.machine.entry = animationState;
        library.machine.states.push_back(
            {animationState,"Default",clipId,{}});
        suite.Require(static_cast<bool>(
                          ui.SetAnimations(std::move(library),true)),
                      "UI animation state machine installs");
        px::ui::VisualStateGroup group;
        group.id = "interaction";
        group.defaultState = "normal";
        group.states = {
            {"normal",{{node,"opacity",px::Variant(1.0)}}},
            {"hover",{{node,"opacity",px::Variant(0.5)}}}};
        group.transitions.push_back(
            {"normal","hover",1.0f,px::ui::VisualStateEase::Linear,
             std::nullopt});
        suite.Require(ui.SetVisualStateGroups({std::move(group)}) &&
                          ui.PreviewAnimation(clipId,0.25f,false) &&
                          ui.SetVisualState("interaction","hover"),
                      "UI animation and Visual State checkpoints become active");
        px::Input input;
        (void)ui.Update(input,640,360,0.4f);
        fixture.runtime.SetUIStateHandler(
            [&ui]{return ui.CaptureRuntimeState();},
            [&ui](const px::ui::UIRuntimeState& state){
                return ui.RestoreRuntimeState(state);
            });
        auto preview=px::preview::CreatePreviewSession(fixture.runtime);
        const auto checkpoint=preview->CaptureCheckpoint();
        suite.Require(checkpoint.accepted&&checkpoint.checkpoint,
                      "PreviewSession captures complete UI runtime state");
        (void)ui.PreviewAnimation(clipId,0.9f,false);
        (void)ui.Update(input,640,360,0.6f);
        suite.Require(preview->RestoreCheckpoint(checkpoint.checkpoint->id).accepted,
                      "PreviewSession restores complete UI runtime state");
        const auto restored=ui.CaptureRuntimeState();
        suite.Expect(restored.animation&&
                         std::abs(restored.animation->position-0.25f)<0.001f&&
                         restored.visualState&&
                         restored.visualState->groups.size()==1&&
                         restored.visualState->groups.front().state=="hover"&&
                         std::abs(restored.visualState->groups.front().elapsed-0.4f)<0.001f,
                     "checkpoint restores UI Animation and Visual State transition progress exactly");
    });

    suite.Run("ApplyPatchAndMemoryCheckpointRestore", [&] {
        RuntimeFixture fixture;
        auto preview = px::preview::CreatePreviewSession(
            fixture.runtime,
            {.checkpointInterval = 2, .maximumCheckpoints = 8});
        const std::string first = Document(
            "linear", 1,
            Json::array({Operation("line-1", "narration", {{"text", "One"}}),
                         Operation("set", "setVariable",
                                   {{"name", "score"}, {"value", "7"}}),
                         Operation("line-2", "narration", {{"text", "Two"}})}));
        const std::uint64_t initialAssetSession = fixture.assets.AssetSession();
        const auto applied = preview->Apply({"linear", 1, first});
        suite.Require(applied.accepted &&
                          applied.status == px::sdk::PreviewSessionStatus::Applied &&
                          fixture.assets.AssetSession() == initialAssetSession + 1,
                      "Apply installs Runtime IR and starts a new asset generation");
        const auto checkpoint = preview->CaptureCheckpoint();
        suite.Require(checkpoint.accepted && checkpoint.checkpoint.has_value(),
                     "facade captures an opaque checkpoint identity");
        suite.Expect(preview->Advance().accepted && preview->Advance().accepted &&
                         fixture.runtime.Variables().Get("score") == 7 &&
                         fixture.runtime.Dialogue().State().fullText == "Two",
                     "advance executes through the shared RuntimeSession");
        suite.Expect(preview->RestoreCheckpoint(checkpoint.checkpoint->id).accepted &&
                         fixture.runtime.Variables().Get("score") == 0 &&
                         fixture.runtime.Dialogue().State().fullText == "One",
                     "memory-applied Runtime IR restores without a VFS script");

        const std::string second = Document(
            "linear", 2,
            Json::array({Operation("line-1", "narration", {{"text", "One+"}}),
                         Operation("set", "setVariable",
                                   {{"name", "score"}, {"value", "9"}}),
                         Operation("line-2", "narration", {{"text", "Two+"}})}));
        const auto patched = preview->Patch({"linear", 2, second});
        suite.Expect(patched.accepted && patched.inPlace &&
                         patched.status == px::sdk::PreviewSessionStatus::Patched &&
                         fixture.assets.AssetSession() == initialAssetSession + 1,
                     "compatible patch preserves execution and asset state in place");
        suite.Expect(preview->Advance().accepted && preview->Advance().accepted &&
                         fixture.runtime.Variables().Get("score") == 9 &&
                         fixture.runtime.Dialogue().State().fullText == "Two+",
                     "patched checkpoint state references the new compiled program");
        const std::string structural = Document(
            "linear", 3,
            Json::array({Operation("line-1", "narration", {{"text", "One+"}}),
                         Operation("inserted", "narration", {{"text", "New"}}),
                         Operation("set", "setVariable",
                                   {{"name", "score"}, {"value", "9"}}),
                         Operation("line-2", "narration", {{"text", "Two+"}})}));
        const auto restarted = preview->Patch({"linear", 3, structural});
        suite.Expect(restarted.accepted && !restarted.inPlace &&
                         restarted.status ==
                             px::sdk::PreviewSessionStatus::Restarted &&
                         fixture.assets.AssetSession() == initialAssetSession + 2,
                     "structural patch restarts a detached asset generation");
        const std::string staleIr = Document(
            "linear", 5,
            Json::array({Operation("line-1", "narration", {{"text", "One+"}}),
                         Operation("set", "setVariable",
                                   {{"name", "score"}, {"value", "9"}}),
                         Operation("line-2", "narration", {{"text", "Two+"}})}));
        const auto stale = preview->Patch({"linear", 5, staleIr});
        suite.Expect(!stale.accepted &&
                         stale.status ==
                             px::sdk::PreviewSessionStatus::RevisionConflict,
                     "patch revisions are monotonic and fail closed");
    });

    suite.Run("BranchAwareStorySeekAndUnsafeReplay", [&] {
        RuntimeFixture fixture;
        auto preview = px::preview::CreatePreviewSession(fixture.runtime);
        const std::string branching = Document(
            "branching", 1,
            Json::array({
                Operation("intro", "narration", {{"text", "Choose"}}),
                Operation("left-choice", "choiceOption",
                          {{"text", "Left"}, {"target", "left"}}),
                Operation("right-choice", "choiceOption",
                          {{"text", "Right"}, {"target", "right"}}),
                Operation("left-label", "label", {{"target", "left"}}),
                Operation("left-value", "setVariable",
                          {{"name", "route"}, {"value", "1"}}),
                Operation("left-end", "jump", {{"target", "end"}}),
                Operation("right-label", "label", {{"target", "right"}}),
                Operation("right-value", "setVariable",
                          {{"name", "route"}, {"value", "2"}}),
                Operation("end-label", "label", {{"target", "end"}}),
                Operation("ending", "narration", {{"text", "Done"}}),
            }));
        suite.Require(preview->Apply({"branching", 1, branching}).accepted,
                     "branching Runtime IR applies");
        const auto unresolved = preview->SeekStory({9, {}});
        suite.Expect(!unresolved.accepted &&
                         unresolved.status ==
                             px::sdk::PreviewSessionStatus::ChoicePathRequired,
                     "seek reports an unresolved choice instead of guessing");
        const auto right = preview->SeekStory({9, {1}});
        suite.Expect(right.accepted &&
                         fixture.runtime.Variables().Get("route") == 2 &&
                         fixture.runtime.Dialogue().State().fullText == "Done" &&
                         preview->State().operationIndex == 9,
                     "seek restores a checkpoint and replays the selected branch");

        const std::string unsafe = Document(
            "unsafe", 1,
            Json::array({Operation("before", "narration", {{"text", "Before"}}),
                         Operation("external", "customNode",
                                   {{"type", "unknown.external"}}),
                         Operation("after", "narration", {{"text", "After"}})}));
        suite.Require(preview->Apply({"unsafe", 1, unsafe}).accepted,
                     "unsafe operation after the first wait does not prevent apply");
        const auto blocked = preview->SeekStory({2, {}});
        suite.Expect(!blocked.accepted &&
                         blocked.status ==
                             px::sdk::PreviewSessionStatus::UnsafeOperation &&
                         !blocked.diagnostics.empty(),
                     "seek refuses operations without an explicit safety contract");
    });

    suite.Run("TimelineViewportStateAndEvents", [&] {
        RuntimeFixture fixture;
        bool resized = false;
        double sampledOpacity = -1.0;
        fixture.runtime.SetAnimationTargetHandler(
            px::animation::TargetKind::UI,
            [&sampledOpacity](const px::animation::TrackBinding& binding,
                              const px::Variant& value) {
                if (binding.target == "panel" && binding.property == "opacity") {
                    if (const auto* number = value.TryGet<double>())
                        sampledOpacity = *number;
                }
                return px::Status::Ok();
            });
        auto preview = px::preview::CreatePreviewSession(
            fixture.runtime,
            {.resize = [&resized](const int width, const int height,
                                  const float scale) {
                resized = width == 960 && height == 540 && scale == 1.5f;
                return px::Status::Ok();
            }});
        const std::string runtimeIr = Document(
            "timeline", 1,
            Json::array({Operation("line", "narration", {{"text", "Line"}})}));
        suite.Require(preview->Apply({"timeline", 1, runtimeIr}).accepted,
                     "timeline test document applies");
        const auto beforeTimeline=preview->CaptureCheckpoint();
        suite.Require(beforeTimeline.accepted&&beforeTimeline.checkpoint,
                      "pre-Timeline checkpoint captures");
        const std::string timeline = Json{
            {"format", "PrismatiXTimeline"},
            {"schemaRevision", 2},
            {"id", "preview.timeline"},
            {"duration", 4.0},
            {"tracks", Json::array({
                {{"id", "panel-opacity"},
                 {"binding", {{"kind", "ui"}, {"target", "panel"},
                              {"property", "opacity"}}},
                 {"keyframes", Json::array({
                     {{"time", 0.0}, {"value", 0.0}, {"easing", "linear"}},
                     {{"time", 4.0}, {"value", 1.0}, {"easing", "linear"}}
                 })}}
            })},
            {"markers", Json::array()},
            {"nestedClips", Json::array()}}
            .dump();
        const auto timelineApplied = preview->ApplyTimeline(
            {"preview.timeline", 1, timeline,
             "memory://preview/timeline.pxtimeline", 0.25, 1.0});
        suite.Require(timelineApplied.accepted &&
                          timelineApplied.playbackHandle != 0,
                      "frontend-neutral facade applies in-memory canonical Timeline content");
        suite.Expect(!preview->RestoreCheckpoint(
                          beforeTimeline.checkpoint->id).accepted,
                     "Timeline hot apply invalidates checkpoints from the previous clip graph");
        const auto handle = timelineApplied.playbackHandle;
        suite.Expect(sampledOpacity > 0.06 && sampledOpacity < 0.07 &&
                         preview->SeekTimeline({handle, 2.5}).accepted &&
                         sampledOpacity > 0.62 && sampledOpacity < 0.63 &&
                         preview->State().timelines.back().seconds == 2.5,
                     "Timeline apply returns a high-frequency scrub handle distinct from Story seek");
        const auto staleTimeline = preview->ApplyTimeline(
            {"preview.timeline", 1, timeline});
        suite.Expect(!staleTimeline.accepted &&
                         staleTimeline.status ==
                             px::sdk::PreviewSessionStatus::RevisionConflict,
                     "Timeline hot apply rejects stale revisions");
        suite.Expect(preview->Resize(960, 540, 1.5f).accepted && resized &&
                         preview->State().viewportWidth == 960,
                     "resize flows through the frontend-neutral callback");
        suite.Expect(preview->Tick(16, 0.016f).accepted,
                     "tick advances shared Runtime semantics");
        const auto events = preview->Events();
        suite.Expect(!events.empty() && preview->Events().empty(),
                     "events preserve order and drain explicitly");
    });

    return suite.Finish();
}
