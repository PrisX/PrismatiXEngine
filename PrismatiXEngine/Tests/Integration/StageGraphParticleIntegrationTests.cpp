#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/IO/VFS.h"
#include "Engine/VN/Runtime/Stage.h"
#include "Engine/UI/Theme.h"
#include "Engine/UI/Widgets.h"
#include "Tests/TestSupport/TestHarness.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <thread>

namespace {

struct Pixel {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0;
};

Pixel ReadPixel(SDL_Surface* surface, const int x, const int y) {
    Pixel pixel;
    (void)SDL_ReadSurfacePixel(surface, x, y, &pixel.r, &pixel.g, &pixel.b,
                               &pixel.a);
    return pixel;
}

bool SaveSolidBmp(const std::filesystem::path& path, const int width,
                  const int height, const Pixel color,
                  const std::optional<Pixel> rightHalf = std::nullopt) {
    SDL_Surface* surface =
        SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return false;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const Pixel value = rightHalf && x >= width / 2
                                    ? *rightHalf
                                    : color;
            if (!SDL_WriteSurfacePixel(surface, x, y, value.r, value.g,
                                       value.b, value.a)) {
                SDL_DestroySurface(surface);
                return false;
            }
        }
    }
    const bool saved = SDL_SaveBMP(surface, path.string().c_str());
    SDL_DestroySurface(surface);
    return saved;
}

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

    suite.Run("ClearCharactersPreservesOtherStageContent", [&] {
        Fixture fixture;
        fixture.stage.SetBackground("Assets/background.webp", false);
        fixture.stage.SetLayer("mist", "Assets/mist.webp", 0.0f, 0.0f,
                               1.0f, 255, 1);
        fixture.stage.SetCharacter("hero", "Assets/hero.webp", 1, false);
        fixture.stage.SetCharacter("friend", "Assets/friend.webp", 3, false);
        fixture.stage.ClearCharacters(true);
        const auto fading = fixture.stage.CaptureState();
        suite.Expect(fading.actors.size() == 2 &&
                         std::ranges::all_of(fading.actors, [](const auto& actor) {
                             return actor.exiting && actor.targetAlpha == 0.0f;
                         }),
                     "clear-all fades every actor together");
        suite.Expect(fixture.stage.BackgroundPath() == "Assets/background.webp" &&
                         fading.layers.size() == 1,
                     "clear-all preserves background and independent layers");
        fixture.stage.Update(1.0f);
        suite.Expect(fixture.stage.CaptureState().actors.empty(),
                     "faded actors leave the stage");
        fixture.stage.SetCharacter("hero", "Assets/hero.webp", 2, false);
        fixture.stage.ClearCharacters(false);
        suite.Expect(fixture.stage.CaptureState().actors.empty() &&
                         fixture.stage.BackgroundPath() == "Assets/background.webp",
                     "instant clear removes actors without clearing the background");
    });

    suite.Run("LayeredPortraitPreloadTracksAssetSession", [&] {
        px::test::TempDirectory fixture("layered-portrait-preload");
        suite.Require(SaveSolidBmp(fixture.path / "body.bmp", 16, 16,
                                   {240, 20, 30, 255}) &&
                          SaveSolidBmp(fixture.path / "replacement.bmp", 16, 16,
                                       {20, 40, 240, 255}),
                      "portrait preload textures are available");
        const auto writePortrait = [&](const char* layer) {
            std::ofstream output(fixture.path / "hero.pxportrait",
                                 std::ios::binary | std::ios::trunc);
            output << "{\"format\":\"PrismatiXLayeredPortrait\","
                   << "\"schemaRevision\":2,\"width\":16,\"height\":16,"
                   << "\"layers\":[{\"path\":\"" << layer
                   << "\",\"width\":16,\"height\":16,\"role\":\"base\"}]}";
            return static_cast<bool>(output);
        };
        suite.Require(writePortrait("body.bmp"),
                      "initial portrait descriptor is available");
        SDL_Surface* surface = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_RGBA32);
        suite.Require(surface != nullptr, "preload surface initializes");
        SDL_Renderer* native = SDL_CreateSoftwareRenderer(surface);
        suite.Require(native != nullptr, "preload renderer initializes");
        {
            px::io::VFS vfs;
            vfs.MountDirectory(fixture.path.string());
            px::graphics::AssetCache assets(native, vfs);
            px::graphics::Renderer2D renderer(native, assets);
            px::vn::Stage stage(renderer, assets);
            const auto waitForTexture = [&](const char* path) {
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(3);
                while (std::chrono::steady_clock::now() < deadline) {
                    assets.BeginFrame();
                    if (assets.HasResidentTexture(path)) return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                return false;
            };
            stage.PreloadCharacter("hero.pxportrait");
            suite.Expect(waitForTexture("body.bmp") &&
                             !assets.HasResidentTexture("hero.pxportrait"),
                         "portrait preloads its image layer, not the descriptor as an image");
            suite.Require(writePortrait("replacement.bmp"),
                          "updated portrait descriptor is available");
            assets.BeginAssetSession();
            stage.PreloadCharacter("hero.pxportrait");
            suite.Expect(waitForTexture("replacement.bmp") &&
                             !assets.HasResidentTexture("body.bmp"),
                         "a new asset session reloads changed portrait layers");
        }
        SDL_DestroyRenderer(native);
        SDL_DestroySurface(surface);
    });

    suite.Run("TextureRectCompositesPortraitLayers", [&] {
        px::test::TempDirectory fixture("ui-layered-portrait");
        suite.Require(SaveSolidBmp(fixture.path / "body.bmp", 16, 16,
                                   {240, 20, 30, 255}) &&
                      SaveSolidBmp(fixture.path / "face.bmp", 8, 16,
                                   {20, 40, 240, 255}),
                      "UI portrait layer textures are available");
        {
            std::ofstream descriptor(fixture.path / "hero.pxportrait");
            descriptor << R"({"format":"PrismatiXLayeredPortrait","schemaRevision":2,
                "width":16,"height":16,"layers":[
                {"path":"body.bmp","width":16,"height":16,"role":"base"},
                {"path":"face.bmp","x":8,"width":8,"height":16,"role":"expression"}]})";
        }
        SDL_Surface* surface = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_RGBA32);
        suite.Require(surface != nullptr, "UI portrait surface initializes");
        SDL_Renderer* native = SDL_CreateSoftwareRenderer(surface);
        suite.Require(native != nullptr, "UI portrait renderer initializes");
        {
            px::io::VFS vfs;
            vfs.MountDirectory(fixture.path.string());
            px::graphics::AssetCache assets(native, vfs);
            px::graphics::Renderer2D renderer(native, assets);
            renderer.SetLogicalSize(64, 64);
            suite.Require(assets.Texture("body.bmp") && assets.Texture("face.bmp"),
                          "UI portrait textures decode before rendering");
            px::ui::TextureRect image("hero.pxportrait");
            suite.Require(image.SetSourceLoader([&](std::string_view path) {
                return vfs.ReadText(path);
            }).IsOk(), "UI portrait descriptor loads through VFS");
            image.Arrange({0, 0, 64, 64});
            SDL_SetRenderDrawColor(native, 0, 0, 0, 255);
            SDL_RenderClear(native);
            image.Draw(renderer, px::ui::Theme{});
            SDL_RenderPresent(native);
            const Pixel body = ReadPixel(surface, 12, 32);
            const Pixel face = ReadPixel(surface, 52, 32);
            suite.Expect(body.r > 220 && body.b < 60 && face.b > 220 &&
                             face.r < 60,
                         "TextureRect draws ordered portrait layers at canvas scale");
        }
        SDL_DestroyRenderer(native);
        SDL_DestroySurface(surface);
    });

    suite.Run("LayeredExpressionCrossfadeKeepsSharedBodyOpaque", [&] {
        px::test::TempDirectory fixture("layered-expression-crossfade");
        suite.Require(SaveSolidBmp(fixture.path / "body.bmp", 16, 16,
                                   {240, 20, 30, 255}) &&
                          SaveSolidBmp(fixture.path / "old-face.bmp", 8, 8,
                                       {20, 40, 240, 255}) &&
                          SaveSolidBmp(fixture.path / "new-face.bmp", 8, 8,
                                       {20, 240, 40, 255}) &&
                          SaveSolidBmp(fixture.path / "ornament.bmp", 4, 4,
                                       {250, 200, 20, 255}),
                      "layered portrait images are available");
        const auto writePortrait = [&](const char* name, const char* face) {
            std::ofstream output(fixture.path / name, std::ios::binary);
            output << "{\"format\":\"PrismatiXLayeredPortrait\","
                   << "\"schemaRevision\":1,\"width\":16,\"height\":16,"
                   << "\"layers\":["
                   << "{\"path\":\"body.bmp\",\"width\":16,\"height\":16},"
                   << "{\"path\":\"" << face
                   << "\",\"width\":8,\"height\":8},"
                   << "{\"path\":\"ornament.bmp\",\"x\":10,"
                   << "\"width\":4,\"height\":4}]}";
            return static_cast<bool>(output);
        };
        suite.Require(writePortrait("old.pxportrait", "old-face.bmp") &&
                          writePortrait("new.pxportrait", "new-face.bmp"),
                      "layered portrait descriptors are available");
        suite.Require(SaveSolidBmp(fixture.path / "other-body.bmp", 16, 16,
                                   {60, 40, 180, 255}),
                      "alternate body is available");
        const auto writeTaggedPortrait = [&](const char* name, const char* body,
                                             const char* face, const char* faceRole) {
            std::ofstream output(fixture.path / name, std::ios::binary);
            output << "{\"format\":\"PrismatiXLayeredPortrait\","
                   << "\"schemaRevision\":2,\"width\":16,\"height\":16,"
                   << "\"layers\":["
                   << "{\"path\":\"" << body
                   << "\",\"width\":16,\"height\":16,\"role\":\"base\"},"
                   << "{\"path\":\"" << face
                   << "\",\"width\":8,\"height\":8,\"role\":\""
                   << faceRole << "\"},"
                   << "{\"path\":\"ornament.bmp\",\"x\":10,"
                   << "\"width\":4,\"height\":4,\"role\":\"base\"}]}";
            return static_cast<bool>(output);
        };
        suite.Require(writeTaggedPortrait("tag-old.pxportrait", "body.bmp",
                                          "old-face.bmp", "expression") &&
                          writeTaggedPortrait("tag-new.pxportrait", "body.bmp",
                                              "new-face.bmp", "expression") &&
                          writeTaggedPortrait("tag-pose.pxportrait", "other-body.bmp",
                                              "new-face.bmp", "expression") &&
                          writeTaggedPortrait("tag-base-face.pxportrait", "body.bmp",
                                              "new-face.bmp", "base"),
                      "revision-2 portrait roles are available");
        SDL_Surface* surface = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_RGBA32);
        suite.Require(surface != nullptr, "portrait surface initializes");
        SDL_Renderer* native = SDL_CreateSoftwareRenderer(surface);
        suite.Require(native != nullptr, "portrait renderer initializes");
        {
            px::io::VFS vfs;
            vfs.MountDirectory(fixture.path.string());
            px::graphics::AssetCache assets(native, vfs);
            px::graphics::Renderer2D renderer(native, assets);
            renderer.SetLogicalSize(64, 64);
            // Portrait descriptor discovery queues asynchronous preloads. Prime
            // this fixture's tiny textures so the first rendered frame tests
            // blending rather than preload scheduling.
            suite.Require(assets.Texture("body.bmp") &&
                              assets.Texture("old-face.bmp") &&
                              assets.Texture("new-face.bmp") &&
                              assets.Texture("ornament.bmp"),
                          "portrait textures decode before the render assertion");
            px::vn::Stage stage(renderer, assets);
            stage.SetCharacter("hero", "old.pxportrait", 2, false);
            stage.SetCharacter("hero", "new.pxportrait", 2, true);
            stage.Update(0.1f);
            SDL_SetRenderDrawColor(native, 0, 0, 0, 255);
            SDL_RenderClear(native);
            stage.Render();
            SDL_RenderPresent(native);
            const Pixel body = ReadPixel(surface, 32, 60);
            const Pixel face = ReadPixel(surface, 28, 51);
            const Pixel ornament = ReadPixel(surface, 35, 50);
            suite.Expect(body.r > 220 && body.g < 60 && body.b < 60,
                         "unchanged body stays opaque during facial crossfade",
                         std::to_string(body.r) + "," + std::to_string(body.g) +
                             "," + std::to_string(body.b));
            suite.Expect(face.g > 70 && face.b > 20,
                         "only changed facial layers blend during the transition",
                         std::to_string(face.r) + "," + std::to_string(face.g) +
                             "," + std::to_string(face.b));
            suite.Expect(ornament.r > 235 && ornament.g > 185 &&
                             ornament.b < 35,
                         "unchanged overlay after the face stays fully opaque",
                         std::to_string(ornament.r) + "," +
                             std::to_string(ornament.g) + "," +
                             std::to_string(ornament.b));
            stage.SetCharacter("hero", "old.pxportrait", 2, false);
            const auto pose = stage.CaptureState().actors;
            suite.Expect(pose.size() == 1 && pose.front().alpha == 255.0f &&
                             pose.front().previousImagePath.empty(),
                         "instant pose replacement cancels an in-progress expression fade");
            stage.SetCharacterAuto("hero", "tag-old.pxportrait", 2);
            stage.SetCharacterAuto("hero", "tag-new.pxportrait", 2);
            const auto autoFace = stage.CaptureState().actors;
            suite.Expect(autoFace.size() == 1 &&
                             autoFace.front().previousImagePath ==
                                 "tag-old.pxportrait" &&
                             autoFace.front().alpha == 0.0f,
                         "auto mode crossfades only a changed expression layer");
            stage.SetCharacterAuto("hero", "tag-pose.pxportrait", 2);
            const auto autoPose = stage.CaptureState().actors;
            suite.Expect(autoPose.size() == 1 &&
                             autoPose.front().previousImagePath.empty() &&
                             autoPose.front().alpha == 255.0f,
                         "auto mode snaps a changed body instead of fading it");
            stage.SetCharacterAuto("hero", "tag-old.pxportrait", 2);
            stage.SetCharacterAuto("hero", "tag-base-face.pxportrait", 2);
            const auto wrongRole = stage.CaptureState().actors;
            suite.Expect(wrongRole.size() == 1 &&
                             wrongRole.front().previousImagePath.empty(),
                         "auto mode never fades changes to base-role layers");
            stage.SetCharacterAuto("hero", "old.pxportrait", 2);
            suite.Expect(stage.CaptureState().actors.front().previousImagePath.empty(),
                         "legacy portraits remain usable and snap in auto mode");
            stage.SetCharacterAuto("newcomer", "tag-old.pxportrait", 2);
            const auto newcomer = stage.CaptureState().actors;
            suite.Expect(std::ranges::any_of(newcomer, [](const auto& actor) {
                return actor.name == "newcomer" && actor.alpha == 0.0f;
            }), "new actors keep the ordinary entrance fade in auto mode");
        }
        SDL_DestroyRenderer(native);
        SDL_DestroySurface(surface);
    });

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

    suite.Run("RendererRetainedPrimitivesAreSafeAndAffine", [&] {
        const auto root = std::filesystem::temp_directory_path() /
                          "prismatix-stage-render-primitives";
        std::error_code error;
        std::filesystem::create_directories(root, error);
        suite.Require(!error &&
                          SaveSolidBmp(root / "regions.bmp", 2, 1,
                                       {240, 20, 30, 255},
                                       Pixel{20, 40, 240, 255}) &&
                          SaveSolidBmp(root / "matrix.bmp", 4, 2,
                                       {30, 230, 70, 255}),
                      "source-region fixture is available");
        SDL_Surface* surface =
            SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_RGBA32);
        suite.Require(surface != nullptr,
                      "retained primitive surface initializes");
        SDL_Renderer* native = SDL_CreateSoftwareRenderer(surface);
        suite.Require(native != nullptr,
                      "retained primitive renderer initializes");
        {
            px::io::VFS vfs;
            vfs.MountDirectory(root.string());
            px::graphics::AssetCache assets(native, vfs);
            px::graphics::Renderer2D renderer(native, assets);
            renderer.SetLogicalSize(64, 64);
            SDL_SetRenderDrawColor(native, 0, 0, 0, 255);
            SDL_RenderClear(native);

            renderer.DrawImageRegion("regions.bmp", {1, 0, 1, 1},
                                     {0, 0, 12, 12});
            SDL_RenderPresent(native);
            const Pixel region = ReadPixel(surface, 4, 4);
            suite.Expect(region.b > 200 && region.r < 50,
                         "pixel source regions do not expose native textures");

            renderer.PushAffineTransform({2, 0, 0, 1, 10, 5});
            renderer.PushAffineTransform({0, 1, -1, 0, 4, 0});
            const px::Vec2 mapped = renderer.TransformPoint({2, 3});
            suite.Expect(std::abs(mapped.x - 12.0f) < 0.001f &&
                             std::abs(mapped.y - 7.0f) < 0.001f,
                         "nested non-uniform affine matrices multiply exactly");
            renderer.PopTransform();
            renderer.PopTransform();

            renderer.PushAffineTransform({1, 0, 0, 1, 20, 10});
            renderer.PushClip({0, 0, 8, 8});
            renderer.DrawRect({0, 0, 16, 16}, {245, 25, 35, 255});
            renderer.PopClip();
            renderer.PopTransform();
            SDL_RenderPresent(native);
            const Pixel clippedInside = ReadPixel(surface, 22, 12);
            const Pixel clippedOutside = ReadPixel(surface, 30, 12);
            suite.Expect(clippedInside.r > 220 && clippedOutside.r < 50,
                         "clip scopes follow the affine transform and stay balanced");

            suite.Require(renderer.BeginComposition(),
                          "software renderer supports retained composition");
            renderer.DrawRect({40, 40, 8, 8}, {20, 230, 60, 255});
            renderer.EndComposition({}, 0.0f, 0.0f, {}, {40, 40, 8, 8});
            SDL_RenderPresent(native);
            const Pixel composed = ReadPixel(surface, 43, 43);
            suite.Expect(composed.g > 200,
                         "a captured group is composited once as a retained texture");

            SDL_SetRenderDrawColor(native, 0, 0, 0, 255);
            SDL_RenderClear(native);
            px::vn::Stage stage(renderer, assets);
            suite.Require(stage.SetGroupNode("matrix-group"),
                          "affine test group is created");
            stage.SetLayerTransform("matrix-leaf", "matrix.bmp", 0.0f,
                                    0.0f, 1.0f, 1.0f, 90.0f, 255, 0);
            suite.Require(stage.SetNodeParent("matrix-leaf", "matrix-group") &&
                              stage.SetNodeTransform(
                                  "matrix-group",
                                  {.x = 30.0f, .y = 30.0f,
                                   .scaleX = 2.0f, .scaleY = 1.0f,
                                   .rotation = 90.0f}),
                          "nested non-uniform Stage transforms are accepted");
            stage.Render();
            SDL_RenderPresent(native);
            const Pixel affineStage = ReadPixel(surface, 28, 34);
            const Pixel obsoleteDecomposition = ReadPixel(surface, 34, 31);
            suite.Expect(affineStage.g > 200 &&
                             obsoleteDecomposition.g < 80,
                         "Stage hierarchy consumes the full affine product instead of decomposed sums");
        }
        SDL_DestroyRenderer(native);
        SDL_DestroySurface(surface);
    });

    suite.Run("ActorsLayersAndParticlesShareOneGlobalOrder", [&] {
        const auto root = std::filesystem::temp_directory_path() /
                          "prismatix-stage-global-order";
        std::error_code error;
        std::filesystem::create_directories(root, error);
        suite.Require(!error &&
                          SaveSolidBmp(root / "actor.bmp", 16, 16,
                                       {240, 20, 30, 255}) &&
                          SaveSolidBmp(root / "layer.bmp", 16, 16,
                                       {20, 40, 240, 255}) &&
                          SaveSolidBmp(root / "field.bmp", 64, 64,
                                       {240, 20, 30, 255}),
                      "global ordering fixtures are available");
        SDL_Surface* surface =
            SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_RGBA32);
        suite.Require(surface != nullptr, "ordering surface initializes");
        SDL_Renderer* native = SDL_CreateSoftwareRenderer(surface);
        suite.Require(native != nullptr, "ordering renderer initializes");
        {
            px::io::VFS vfs;
            vfs.MountDirectory(root.string());
            px::graphics::AssetCache assets(native, vfs);
            px::graphics::Renderer2D renderer(native, assets);
            renderer.SetLogicalSize(64, 64);
            px::vn::Stage stage(renderer, assets);
            stage.SetCharacter("hero", "actor.bmp", 2, false);
            stage.SetLayer("between", "layer.bmp", 24.0f, 48.0f,
                           1.0f, 255, 1);

            SDL_SetRenderDrawColor(native, 0, 0, 0, 255);
            SDL_RenderClear(native);
            stage.Render();
            SDL_RenderPresent(native);
            Pixel overlap = ReadPixel(surface, 32, 56);
            suite.Expect(overlap.r > 220 && overlap.b < 60,
                         "a lower-z positive layer can render behind an actor");

            suite.Require(stage.SetNodeOrder("between", 3, 2),
                          "layer order can cross the actor slot adapter");
            SDL_SetRenderDrawColor(native, 0, 0, 0, 255);
            SDL_RenderClear(native);
            stage.Render();
            SDL_RenderPresent(native);
            overlap = ReadPixel(surface, 32, 56);
            suite.Expect(overlap.b > 220 && overlap.r < 60,
                         "a higher-z layer renders in front of the actor");

            stage.ClearCharacter("hero", false);
            stage.SetLayer("field", "field.bmp", 0.0f, 0.0f, 1.0f,
                           255, 0);
            px::vn::ParticleEmitterSpec particle;
            particle.seed = 9;
            particle.z = 1;
            particle.rate = 1.0f;
            particle.maxParticles = 1;
            particle.burst = 1;
            particle.loop = false;
            particle.advanced = true;
            particle.spawnShape = px::vn::ParticleSpawnShape::Point;
            particle.positionX = {0.5f, 0.5f};
            particle.positionY = {0.5f, 0.5f};
            particle.velocityX = {0.0f, 0.0f};
            particle.velocityY = {0.0f, 0.0f};
            particle.lifetime = {5.0f, 5.0f};
            particle.scale = {1.0f, 1.0f};
            particle.initialOpacity = {1.0f, 1.0f};
            particle.colorOverLifetime = {
                {0.0f, {20, 240, 40, 255}},
                {1.0f, {20, 240, 40, 255}}};
            suite.Require(stage.SetParticleEmitter("spark", particle),
                          "ordering particle validates");
            stage.Update(0.2f);
            SDL_SetRenderDrawColor(native, 0, 0, 0, 255);
            SDL_RenderClear(native);
            stage.Render();
            SDL_RenderPresent(native);
            const Pixel particleOverLayer = ReadPixel(surface, 36, 36);
            suite.Expect(particleOverLayer.g > particleOverLayer.r,
                         "a higher-z particle emitter renders over a layer");

            stage.SetNodeOrder("field", 2, 3);
            SDL_SetRenderDrawColor(native, 0, 0, 0, 255);
            SDL_RenderClear(native);
            stage.Render();
            SDL_RenderPresent(native);
            const Pixel layerOverParticle = ReadPixel(surface, 36, 36);
            suite.Expect(layerOverParticle.r > 220 && layerOverParticle.g < 80,
                         "a higher-z layer renders over a particle emitter");
        }
        SDL_DestroyRenderer(native);
        SDL_DestroySurface(surface);
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
