#include "Engine/Animation/Timeline.h"
#include "Engine/VN/Scenario/ScenarioDocument.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace {

int failures = 0;
void Check(bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void TestLargeScenarioScale() {
    px::vn::scenario::ScenarioDocument document;
    document.id = px::Uuid::Random();
    document.name = "10k acceptance";
    document.nodes.reserve(10000);
    document.edges.reserve(9999);
    for (int index = 0; index < 10000; ++index) {
        px::vn::scenario::ScenarioNode node{
            px::Uuid::Random(), "say",
            {{"textId", px::Uuid::Random().ToString()},
             {"value", std::string("line ") + std::to_string(index)}}};
        if (index == 0) document.entry = node.id;
        if (index > 0)
            document.edges.push_back(
                {px::Uuid::Random(), document.nodes.back().id, "flow", node.id, "in"});
        document.nodes.push_back(std::move(node));
    }
    const auto encoded = px::vn::scenario::WriteScenario(document);
    const auto parsed = px::vn::scenario::ParseScenario(encoded, "large.pxscenario");
    Check(parsed && parsed.Value().nodes.size() == 10000 &&
              px::vn::scenario::ValidateScenario(parsed.Value()).Valid(),
          "10,000-line Scenario should serialize, parse, and validate");
}

void TestAcceleratedEightHourSoak() {
    px::animation::AnimationClip clip;
    clip.id = px::Uuid::FromName("acceptance-soak");
    clip.name = "Eight hour soak";
    clip.duration = 2.0f;
    clip.loop = true;
    clip.tracks.push_back({{px::animation::TargetKind::Camera, "main", "zoom"},
                           {{0.0f, 1.0, px::animation::Curve::Linear},
                            {2.0f, 1.1, px::animation::Curve::EaseInOut}}});
    px::animation::TimelinePlayer player;
    std::uint64_t samples = 0;
    player.SetApply([&](const px::animation::TrackBinding&, const px::Variant&) {
        ++samples;
        return px::Status::Ok();
    });
    Check(static_cast<bool>(player.Register(clip)), "soak animation should register");
    const auto handle = player.Play(clip.id);
    constexpr int updates = 8 * 60 * 60 * 4;
    for (int index = 0; index < updates; ++index) player.Update(0.25f);
    const auto state = player.CaptureState();
    Check(player.Playing(handle) && state.size() == 1 &&
              state.front().loopIteration >= 14399 && samples >= updates,
          "accelerated eight-hour timeline soak should remain bounded and playing");
}

}  // namespace

int main() {
    TestLargeScenarioScale();
    TestAcceleratedEightHourSoak();
    if (failures == 0) std::cout << "All performance and stress tests passed.\n";
    return failures == 0 ? 0 : 1;
}
