#include "Engine/Animation/Timeline.h"
#include "Tests/TestSupport/TestHarness.h"

#include <cstdint>

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
    return suite.Finish();
}
