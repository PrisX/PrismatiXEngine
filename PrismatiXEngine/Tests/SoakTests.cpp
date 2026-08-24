#include "Engine/Animation/Timeline.h"
#include "Engine/Preview/PreviewProtocolV2.h"
#include "Tests/TestSupport/TestHarness.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

int main() {
    px::test::Suite suite("Soak");
    suite.Run("AcceleratedEightHourTimeline_RemainsBoundedAndRestorable", [&] {
        px::animation::AnimationClip clip;
        clip.id = px::Uuid::FromName("PrismatiX.Soak.Timeline");
        clip.name = "Eight hour soak";
        clip.duration = 2.0f;
        clip.loop = true;
        clip.tracks.push_back(
            {{px::animation::TargetKind::Camera, "main", "zoom"},
             {{0.0f, 1.0, px::animation::Curve::Linear},
              {2.0f, 1.1, px::animation::Curve::EaseInOut}}});

        px::animation::TimelinePlayer player;
        std::uint64_t samples = 0;
        player.SetApply(
            [&](const px::animation::TrackBinding&, const px::Variant&) {
                ++samples;
                return px::Status::Ok();
            });
        suite.Expect(static_cast<bool>(player.Register(clip)),
                     "soak animation registers");
        const auto handle = player.Play(clip.id);
        constexpr int updates = 8 * 60 * 60 * 4;
        for (int index = 0; index < updates; ++index) player.Update(0.25f);
        const auto state = player.CaptureState();
        suite.Expect(player.Playing(handle) && state.size() == 1 &&
                         state.front().loopIteration >= 14399 && samples >= updates,
                     "accelerated eight-hour state stays bounded and playing");
        suite.Expect(player.RestoreState(state) && player.Playing(handle),
                     "soak checkpoint restores without a second runtime path");
    });
    suite.Run("FiftyThousandAssetEnvelope_RemainsCompleteAndCorrelated", [&] {
        using Json = nlohmann::json;
        constexpr std::size_t assetCount = 50'000;
        Json runtimeFiles = Json::array();
        runtimeFiles.get_ref<Json::array_t&>().reserve(assetCount);
        const std::string sha256(64, 'a');
        for (std::size_t index = 0; index < assetCount; ++index) {
            runtimeFiles.push_back(
                {{"virtualPath", "/project/Assets/stress/asset-" +
                                     std::to_string(index) + ".bin"},
                 {"sha256", sha256},
                 {"kind", "binary"},
                 {"byteLength", static_cast<std::uint64_t>(1024 + index % 4096)}});
        }
        const std::string runtimeIr =
            Json{{"format", "PrismatiXRuntimeIR"},
                 {"schemaRevision", 1},
                 {"documentId", "stress-scene"},
                 {"committedRevision", 1},
                 {"operations", Json::array()}}
                .dump();
        const std::string envelope =
            Json{{"protocol", "PrismatiXPreviewProtocol"},
                 {"schemaRevision", 2},
                 {"protocolVersion", 2},
                 {"type", "apply"},
                 {"sessionId", "stress-session"},
                 {"requestId", "stress-apply-1"},
                 {"documentId", "stress-scene"},
                 {"revision", 1},
                 {"payload",
                  {{"runtimeIr", runtimeIr},
                   {"runtimeFiles", std::move(runtimeFiles)}}}}
                .dump();

        px::preview::PreviewProtocolV2 protocol;
        const auto start = std::chrono::steady_clock::now();
        const auto accepted = protocol.AcceptApply(envelope, false);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        suite.Require(accepted.Accepted(),
                      "50,000-entry active asset graph is accepted");
        const Json retained = Json::parse(accepted.request->runtimeFilesJson);
        suite.Expect(retained.size() == assetCount,
                     "every active asset entry survives protocol validation",
                     std::to_string(retained.size()));
        protocol.CommitApply(*accepted.request);
        suite.Expect(protocol.SessionId() == "stress-session" &&
                         protocol.DocumentId() == "stress-scene" &&
                         protocol.Revision() == 1,
                     "large asset apply preserves session identity and revision");
        suite.Expect(elapsed < std::chrono::seconds(60),
                     "50,000-entry validation stays inside the stress budget");
        std::cout << "50,000 asset envelope: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                  << " ms\n";
    });
    return suite.Finish();
}
