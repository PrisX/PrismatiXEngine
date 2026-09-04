#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/IO/VFS.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Tests/TestSupport/TestHarness.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <ranges>
#include <string>

namespace {

struct Fixture {
    px::io::VFS vfs;
    px::graphics::AssetCache assets{nullptr, vfs};
    px::graphics::Renderer2D renderer{nullptr, assets};
    px::vn::Stage stage{renderer, assets};
};

void ExpectSameParticles(px::test::Suite& suite,
                         const std::vector<px::vn::ParticleSample>& left,
                         const std::vector<px::vn::ParticleSample>& right) {
    suite.Require(left.size() == right.size(),
                  "restored emitter reproduces the particle count");
    for (std::size_t index = 0; index < left.size(); ++index) {
        suite.Expect(left[index].id == right[index].id &&
                         left[index].position == right[index].position &&
                         left[index].extent == right[index].extent &&
                         left[index].color == right[index].color &&
                         left[index].rotation == right[index].rotation &&
                         left[index].source == right[index].source &&
                         left[index].texture == right[index].texture,
                     "restored emitter reproduces deterministic samples");
    }
}

}  // namespace

int main() {
    px::test::Suite suite("StageGraphParticleIntegration");

    suite.Run("LegacyLayersAreGraphLeaves", [&] {
        Fixture fixture;
        fixture.stage.SetLayer("mist", "Assets/mist.webp", 10.0f, 20.0f,
                               1.25f, 192, -2);
        suite.Require(fixture.stage.SetGroupNode("weather"),
                      "group node is created");
        suite.Expect(fixture.stage.SetNodeParent("mist", "weather"),
                     "legacy layer can be parented without replacement");
        suite.Expect(fixture.stage.SetNodeTransform(
                         "weather", {.x = 5.0f, .y = -3.0f, .scaleX = 0.9f,
                                     .scaleY = 1.1f, .rotation = 12.0f,
                                     .opacity = 0.8f}),
                     "group transform is accepted");
        suite.Expect(!fixture.stage.SetNodeParent("weather", "weather"),
                     "self cycles are rejected");
        suite.Expect(!fixture.stage.SetNodeParent("weather", "mist"),
                     "leaf nodes cannot become parents");
        suite.Expect(!fixture.stage.SetNodeOrder("weather", 0, 1'000'001),
                     "pathological node order values are rejected");

        const auto nodes = fixture.stage.SnapshotNodes();
        suite.Require(nodes.size() == 2, "layer and group are both captured");
        const auto group = std::ranges::find_if(nodes, [](const auto& node) {
            return node.name == "weather";
        });
        const auto image = std::ranges::find_if(nodes, [](const auto& node) {
            return node.name == "mist";
        });
        suite.Require(group != nodes.end() && image != nodes.end(),
                      "named graph nodes survive capture");
        suite.Expect(group->kind == px::vn::Stage::NodeKind::Group &&
                         group->children == std::vector<std::string>{"mist"} &&
                         image->kind == px::vn::Stage::NodeKind::Image &&
                         image->parent == "weather",
                     "parent/children and node kinds are explicit");

        const auto state = fixture.stage.CaptureState();
        Fixture restored;
        const auto restoreStatus = restored.stage.RestoreState(state);
        suite.Require(static_cast<bool>(restoreStatus),
                      "stage graph restores with legacy payloads");
        suite.Expect(restored.stage.SnapshotLayers().size() == 1 &&
                         restored.stage.SnapshotNodes().size() == 2,
                     "SetLayer compatibility payload remains available");
    });

    suite.Run("ParticleStateIsDeterministicAndValidated", [&] {
        Fixture original;
        px::vn::ParticleEmitterSpec spec;
        spec.preset = px::vn::ParticlePreset::Sakura;
        spec.seed = 0x12345678u;
        spec.rate = 70.0f;
        spec.maxParticles = 180;
        spec.wind = 0.4f;
        spec.speed = 1.2f;
        suite.Require(original.stage.SetParticleEmitter("petals", spec),
                      "seeded emitter is accepted");
        for (int tick = 0; tick < 123; ++tick)
            original.stage.Update(1.0f / 60.0f);

        const auto captured = original.stage.CaptureState();
        Fixture restored;
        const auto restoreStatus = restored.stage.RestoreState(captured);
        suite.Require(static_cast<bool>(restoreStatus),
                      "emitter state restores");
        ExpectSameParticles(suite,
                            original.stage.SampleParticles("petals", 1280, 720),
                            restored.stage.SampleParticles("petals", 1280, 720));

        original.stage.Update(0.137f);
        restored.stage.Update(0.137f);
        ExpectSameParticles(suite,
                            original.stage.SampleParticles("petals", 1280, 720),
                            restored.stage.SampleParticles("petals", 1280, 720));

        auto corrupt = captured;
        corrupt.particleEmitters.front().tickRemainder = 1.5;
        Fixture rejected;
        suite.Expect(!static_cast<bool>(rejected.stage.RestoreState(corrupt)),
                     "invalid saved emitter clocks are rejected");

        corrupt = captured;
        corrupt.particleEmitters.front().ticks =
            (std::numeric_limits<std::uint64_t>::max)();
        suite.Expect(!static_cast<bool>(rejected.stage.RestoreState(corrupt)),
                     "overflow-prone saved emitter clocks are rejected");

        px::vn::ParticleEmitterSpec oneShot;
        oneShot.seed = 7;
        oneShot.burst = 32;
        oneShot.maxParticles = 64;
        oneShot.loop = false;
        oneShot.lifetime = {0.5f, 0.5f};
        oneShot.advanced = true;
        suite.Require(original.stage.SetParticleEmitter("one-shot", oneShot),
                      "one-shot burst emitter is accepted");
        for (int tick = 0; tick < 4; ++tick) original.stage.Update(0.25f);
        suite.Expect(original.stage.SampleParticles("one-shot", 1280, 720).empty(),
                     "non-looping burst without a duration does not emit continuously");
    });

    suite.Run("TexturedAdvancedParticlesRestoreAndBatchAtHighCount", [&] {
        px::io::VFS vfs;
        vfs.MountDirectory(PRISMATIX_RESOURCE_ROOT);
        SDL_Surface* surface =
            SDL_CreateSurface(640, 360, SDL_PIXELFORMAT_RGBA32);
        suite.Require(surface != nullptr,
                      "particle stress surface should initialize");
        SDL_Renderer* native = SDL_CreateSoftwareRenderer(surface);
        suite.Require(native != nullptr,
                      "particle stress renderer should initialize");
        {
        px::graphics::AssetCache assets(native, vfs);
        px::graphics::Renderer2D renderer(native, assets);
        renderer.SetLogicalSize(640, 360);
        px::vn::Stage stage(renderer, assets);

        px::vn::ParticleEmitterSpec spec;
        spec.preset = px::vn::ParticlePreset::Sakura;
        spec.seed = 0x98765432u;
        spec.rate = 1.0f;
        spec.maxParticles = 20'000;
        spec.z = -1;
        spec.texture = "Branding/PrismatiXEngine_Logo.png";
        spec.atlasColumns = 2;
        spec.atlasRows = 2;
        spec.atlasFrameCount = 4;
        spec.spawnShape = px::vn::ParticleSpawnShape::Ellipse;
        spec.positionX = {0.1f, 0.9f};
        spec.positionY = {0.1f, 0.4f};
        spec.velocityX = {-25.0f, 35.0f};
        spec.velocityY = {20.0f, 80.0f};
        spec.accelerationX = {-2.0f, 2.0f};
        spec.accelerationY = {5.0f, 15.0f};
        spec.lifetime = {30.0f, 40.0f};
        spec.rotation = {-30.0f, 30.0f};
        spec.angularVelocity = {-120.0f, 120.0f};
        spec.scale = {0.25f, 1.25f};
        spec.initialOpacity = {0.5f, 1.0f};
        spec.scaleOverLifetime = {{0.0f, 0.25f}, {0.2f, 1.0f},
                                  {1.0f, 0.1f}};
        spec.opacityOverLifetime = {{0.0f, 0.0f}, {0.05f, 1.0f},
                                    {0.8f, 1.0f}, {1.0f, 0.0f}};
        spec.colorOverLifetime = {
            {0.0f, {255, 180, 210, 255}},
            {1.0f, {180, 210, 255, 128}}};
        spec.gravity = 18.0f;
        spec.wind = 6.0f;
        spec.variation = 0.35f;
        spec.burst = 20'000;
        spec.loop = false;
        spec.duration = 1.0f;
        spec.advanced = true;
        suite.Require(stage.SetParticleEmitter("textured-stress", spec),
                      "production textured emitter should validate");
        stage.Update(1.0f / 60.0f);
        const auto samples =
            stage.SampleParticles("textured-stress", 640, 360);
        suite.Require(samples.size() >= 19'999 &&
                          samples.front().texture == spec.texture &&
                          samples.front().source.w == 0.5f,
                      "burst creates deterministic texture-atlas samples");
        const auto checkpoint = stage.CaptureState();
        px::vn::Stage restored(renderer, assets);
        suite.Require(static_cast<bool>(restored.RestoreState(checkpoint)),
                      "advanced particle state restores");
        ExpectSameParticles(
            suite, samples,
            restored.SampleParticles("textured-stress", 640, 360));

        const auto started = std::chrono::steady_clock::now();
        stage.Render();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        suite.Expect(renderer.GeometryBatchCount() == 1 &&
                         renderer.GeometryBatchItems() >= 19'999,
                     "high-count particles use one batched geometry submission");
        suite.Expect(elapsed < std::chrono::seconds(5),
                     "20k textured particle stress frame stays within the acceptance budget");
        }
        SDL_DestroyRenderer(native);
        SDL_DestroySurface(surface);
    });

    suite.Run("CorruptedGraphCannotRestore", [&] {
        Fixture fixture;
        fixture.stage.SetGroupNode("a");
        fixture.stage.SetGroupNode("b", "a");
        auto state = fixture.stage.CaptureState();
        for (auto& node : state.nodes) {
            if (node.name == "a") {
                node.parent = "b";
                node.children.clear();
            } else if (node.name == "b") {
                node.children = {"a"};
            }
        }
        Fixture rejected;
        suite.Expect(!static_cast<bool>(rejected.stage.RestoreState(state)),
                     "cyclic graph state is rejected before graph commit");
        state = fixture.stage.CaptureState();
        state.nodes.front().kind =
            static_cast<px::vn::Stage::NodeKind>(255);
        suite.Expect(!static_cast<bool>(rejected.stage.RestoreState(state)),
                     "unknown saved node kinds are rejected");
    });

    return suite.Finish();
}
