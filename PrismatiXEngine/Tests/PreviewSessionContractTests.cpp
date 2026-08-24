#include "Engine/Audio/AudioEngine.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/IO/VFS.h"
#include "Engine/Preview/PreviewSessionFactory.h"
#include "Engine/Session/RuntimeSession.h"
#include "Tests/TestSupport/TestHarness.h"

#include <nlohmann/json.hpp>

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
                {"schemaRevision", 1},
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
        const auto applied = preview->Apply({"linear", 1, first});
        suite.Require(applied.accepted &&
                         applied.status == px::sdk::PreviewSessionStatus::Applied,
                     "Apply installs Runtime IR through the stable facade");
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
                         patched.status == px::sdk::PreviewSessionStatus::Patched,
                     "compatible patch preserves execution state in place");
        suite.Expect(preview->Advance().accepted && preview->Advance().accepted &&
                         fixture.runtime.Variables().Get("score") == 9 &&
                         fixture.runtime.Dialogue().State().fullText == "Two+",
                     "patched checkpoint state references the new compiled program");
        const std::string staleIr = Document(
            "linear", 4,
            Json::array({Operation("line-1", "narration", {{"text", "One+"}}),
                         Operation("set", "setVariable",
                                   {{"name", "score"}, {"value", "9"}}),
                         Operation("line-2", "narration", {{"text", "Two+"}})}));
        const auto stale = preview->Patch({"linear", 4, staleIr});
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
        px::animation::AnimationClip clip;
        clip.id = px::Uuid::FromName("PreviewSession.Contract.Timeline");
        clip.name = "Contract";
        clip.duration = 4.0f;
        suite.Expect(static_cast<bool>(fixture.runtime.Timeline().Register(clip)),
                     "test timeline registers");
        const auto handle = fixture.runtime.Timeline().Play(clip.id);
        suite.Expect(handle != 0 &&
                         preview->SeekTimeline({handle, 2.5}).accepted &&
                         preview->State().timelines.front().seconds == 2.5,
                     "Timeline seek is distinct from Story seek");
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
