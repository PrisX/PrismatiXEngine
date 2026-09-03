#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Graphics/Screenshot.h"
#include "Engine/IO/VFS.h"
#include "Engine/Platform/Input.h"
#include "Engine/Text/Typography.h"
#include "Engine/UI/GalgameUI.h"
#include "Engine/VN/Runtime/Stage.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

namespace {

std::uint64_t SurfaceHash(const SDL_Surface* surface) {
    if (!surface || !surface->pixels || surface->pitch <= 0 || surface->h <= 0)
        return 0;
    const auto* bytes = static_cast<const std::uint8_t*>(surface->pixels);
    const std::size_t count = static_cast<std::size_t>(surface->pitch) *
                              static_cast<std::size_t>(surface->h);
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t index = 0; index < count; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

struct Pixel {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0;
};

struct FrameEvidence {
    std::uint64_t hash = 0;
    Pixel corner;
    Pixel center;
    Pixel edge;
};

Pixel ReadLogicalPixel(SDL_Surface* surface, const int logicalX,
                       const int logicalY, const int logicalWidth,
                       const int logicalHeight) {
    Pixel pixel;
    if (!surface || surface->w <= 0 || surface->h <= 0) return pixel;
    const int x = std::clamp(logicalX * surface->w / logicalWidth, 0,
                             surface->w - 1);
    const int y = std::clamp(logicalY * surface->h / logicalHeight, 0,
                             surface->h - 1);
    (void)SDL_ReadSurfacePixel(surface, x, y, &pixel.r, &pixel.g, &pixel.b,
                               &pixel.a);
    return pixel;
}

void SaveVisualArtifact(SDL_Renderer* renderer, const std::string_view name) {
    const char* outputRoot = SDL_getenv("PRISMATIX_VISUAL_OUTPUT_DIR");
    if (!outputRoot || !*outputRoot) return;
    std::error_code error;
    const std::filesystem::path root(outputRoot);
    std::filesystem::create_directories(root, error);
    if (error) return;
    const auto png = px::graphics::CaptureThumbnailPng(renderer, 640, 360);
    if (png.empty()) return;
    std::ofstream output(root / (std::string(name) + ".png"),
                         std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(png.data()),
                 static_cast<std::streamsize>(png.size()));
}

FrameEvidence CaptureFrame(SDL_Renderer* renderer, const std::string_view name) {
    SaveVisualArtifact(renderer, name);
    FrameEvidence evidence;
    SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr);
    if (!surface) return evidence;
    evidence.hash = SurfaceHash(surface);
    evidence.corner = ReadLogicalPixel(surface, 8, 8, 640, 360);
    evidence.center = ReadLogicalPixel(surface, 320, 180, 640, 360);
    evidence.edge = ReadLogicalPixel(surface, 190, 180, 640, 360);
    SDL_DestroySurface(surface);
    return evidence;
}

void ClearFrame(SDL_Renderer* renderer) {
    SDL_SetRenderDrawColor(renderer, 7, 11, 19, 255);
    SDL_RenderClear(renderer);
}

bool SaveFixtureSurface(const std::filesystem::path& path,
                        const bool alternate, const bool rule = false) {
    SDL_Surface* surface = SDL_CreateSurface(640, 360, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return false;
    for (int y = 0; y < surface->h; ++y) {
        for (int x = 0; x < surface->w; ++x) {
            Pixel color;
            if (rule) {
                const auto value = static_cast<std::uint8_t>(
                    (x * 255) / std::max(1, surface->w - 1));
                color = {value, value, value, 255};
            } else if (alternate) {
                color = {static_cast<std::uint8_t>(40 + y / 3),
                         static_cast<std::uint8_t>(90 + x / 7), 205, 255};
            } else if (x >= 190 && x < 450 && y >= 85 && y < 285) {
                color = {230, 45, 55, 255};
            } else {
                color = {230, static_cast<std::uint8_t>(210 + x / 16),
                         static_cast<std::uint8_t>(180 + y / 8), 255};
            }
            if (!SDL_WriteSurfacePixel(surface, x, y, color.r, color.g,
                                       color.b, color.a)) {
                SDL_DestroySurface(surface);
                return false;
            }
        }
    }
    const bool saved = SDL_SaveBMP(surface, path.string().c_str());
    SDL_DestroySurface(surface);
    return saved;
}

}  // namespace

int main() {
#if defined(__EMSCRIPTEN__)
    std::cout << "PASS: GPU compositor is intentionally native-only\n";
    return 0;
#else
    if (!SDL_Init(SDL_INIT_VIDEO) || !TTF_Init()) {
        std::cerr << "FAIL: SDL video/text initialization: " << SDL_GetError()
                  << '\n';
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow(
        "PrismatiX GPU conformance", 640, 360,
        SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        std::cerr << "FAIL: GPU conformance window: " << SDL_GetError() << '\n';
        TTF_Quit();
        SDL_Quit();
        return 1;
    }

    int result = 0;
    std::string driver;
    px::graphics::GraphicsDevice device;
    if (!device.Create(window, px::graphics::GraphicsTier::GpuEffects) ||
        !device.GPU() || !device.Renderer() || device.ShaderFormats() == 0 ||
        device.Driver().empty()) {
        std::cerr << "FAIL: required SDL_GPU D3D12/Metal/Vulkan device: "
                  << SDL_GetError() << '\n';
        result = 1;
    } else {
        driver = device.Driver();
        px::io::VFS vfs;
        vfs.MountDirectory(PRISMATIX_RESOURCE_ROOT);
        px::graphics::AssetCache assets(device.Renderer(), vfs);
        px::graphics::Renderer2D renderer(device.Renderer(), assets, true);
        renderer.SetLogicalSize(640, 360);
        const std::string font = "Fonts/NotoSansTC-Bold.ttf";
        const auto vertical = renderer.LayoutText(
            "Aあ\nB", font, 24, 48, px::text::TextOrientation::Vertical);
        if (vertical.Clusters().size() != 3 || vertical.Size().x <= 0.0f ||
            vertical.Size().y <= 0.0f ||
            vertical.Clusters()[0].bounds.x <=
                vertical.Clusters()[2].bounds.x ||
            !vertical.Clusters()[0].rotated ||
            vertical.Clusters()[1].rotated) {
            std::cerr << "FAIL: vertical shaping/newline/rotation layout is inconsistent\n";
            result = 1;
        }
        const auto emojiBoundaries =
            px::text::GraphemeBoundaries("👩‍👩‍👧‍👦X");
        if (emojiBoundaries.size() != 3) {
            std::cerr << "FAIL: emoji ZWJ is not one caret/typewriter cluster\n";
            result = 1;
        }
        const auto rubyBase = renderer.LayoutText("前漢字後", font, 24);
        const px::Rect rubyBounds = rubyBase.BoundsForRange(
            std::string("前").size(), std::string("漢字").size());
        if (rubyBounds.w <= 0.0f || rubyBounds.h <= 0.0f) {
            std::cerr << "FAIL: ruby base range is not derived from shaped clusters\n";
            result = 1;
        }
        const auto drawFixture = [&] {
            renderer.DrawRect({0, 0, 640, 360}, {230, 230, 210, 255});
            renderer.DrawRect({190, 85, 260, 200}, {230, 45, 55, 255});
        };

        const auto renderCompositorEffect = [&](const std::string_view name,
                                                const px::graphics::StagePostEffects& effect) {
            ClearFrame(device.Renderer());
            if (!renderer.BeginStageLayer()) return FrameEvidence{};
            drawFixture();
            renderer.EndStageLayer(effect);
            return CaptureFrame(device.Renderer(), name);
        };
        const FrameEvidence baseline = renderCompositorEffect("gpu-baseline", {});
        const FrameEvidence blur = renderCompositorEffect(
            "gpu-blur", {.blur = 0.75f});
        const FrameEvidence vignette = renderCompositorEffect(
            "gpu-vignette", {.vignette = 0.85f});
        const FrameEvidence colorGrade = renderCompositorEffect(
            "gpu-color-grade", {.colorGrade = 0.65f});
        const std::set<std::uint64_t> gpuHashes{
            baseline.hash, blur.hash, vignette.hash, colorGrade.hash};
        if (baseline.hash == 0 || gpuHashes.size() != 4 ||
            vignette.corner.r >= baseline.corner.r ||
            colorGrade.center.g <= baseline.center.g ||
            blur.edge.g == baseline.edge.g) {
            std::cerr << "FAIL: blur, vignette, and color-grade must each produce "
                         "distinct pixel evidence\n";
            result = 1;
        }

        px::graphics::ScreenEffectDefinition tileDefinition{
            .id = "test-tile-flip",
            .operation = "tiles",
            .columns = 7,
            .rows = 5,
            .stagger = 0.4f,
            .order = "reverse"};
        px::graphics::ScreenEffectDefinition invalidDefinition{
            .id = "invalid effect", .operation = "tiles"};
        if (!renderer.RegisterScreenEffect(tileDefinition) ||
            renderer.RegisterScreenEffect(invalidDefinition) ||
            !renderer.HasScreenEffect("fade") ||
            !renderer.HasScreenEffect("crossfade") ||
            !renderer.HasScreenEffect("slide-left") ||
            !renderer.HasScreenEffect("slide-right")) {
            std::cerr << "FAIL: screen transition registry validation or built-ins are incomplete\n";
            result = 1;
        }
        if (!renderer.BeginFrame({220, 35, 45, 255})) {
            std::cerr << "FAIL: screen transition outgoing target could not begin\n";
            result = 1;
        } else {
            renderer.DrawRect({0, 0, 640, 360}, {220, 35, 45, 255});
            renderer.EndFrame();
        }
        const FrameEvidence outgoingFrame =
            CaptureFrame(device.Renderer(), "transition-outgoing");
        const auto tileHandle =
            renderer.PlayScreenEffect("test-tile-flip", 1.0f);
        renderer.UpdateScreenEffects(0.5f);
        if (!tileHandle || !renderer.ScreenEffectPlaying(tileHandle) ||
            !renderer.BeginFrame({30, 75, 220, 255})) {
            std::cerr << "FAIL: custom tile transition could not start\n";
            result = 1;
        } else {
            renderer.DrawRect({0, 0, 640, 360}, {30, 75, 220, 255});
            renderer.EndFrame();
        }
        const FrameEvidence tileFrame =
            CaptureFrame(device.Renderer(), "transition-tile-midpoint");
        renderer.UpdateScreenEffects(0.6f);
        if (renderer.ScreenEffectState(tileHandle) !=
                px::graphics::ScreenEffectStatus::Completed ||
            !renderer.BeginFrame({30, 75, 220, 255})) {
            std::cerr << "FAIL: screen transition did not complete deterministically\n";
            result = 1;
        } else {
            renderer.DrawRect({0, 0, 640, 360}, {30, 75, 220, 255});
            renderer.EndFrame();
        }
        const FrameEvidence incomingFrame =
            CaptureFrame(device.Renderer(), "transition-incoming");
        if (outgoingFrame.hash == 0 || incomingFrame.hash == 0 ||
            tileFrame.hash == outgoingFrame.hash ||
            tileFrame.hash == incomingFrame.hash ||
            outgoingFrame.hash == incomingFrame.hash) {
            std::cerr << "FAIL: outgoing/incoming snapshots were not composited by the native tile operator\n";
            result = 1;
        }
        const auto stoppedHandle = renderer.PlayScreenEffect("fade", 0.5f);
        if (!stoppedHandle || !renderer.StopScreenEffect(stoppedHandle) ||
            renderer.ScreenEffectState(stoppedHandle) !=
                px::graphics::ScreenEffectStatus::Stopped) {
            std::cerr << "FAIL: screen transition stop lifecycle is incomplete\n";
            result = 1;
        }
        const auto cancelledHandle =
            renderer.PlayScreenEffect("crossfade", 0.5f);
        if (!cancelledHandle ||
            !renderer.CancelScreenEffect(cancelledHandle) ||
            renderer.ScreenEffectState(cancelledHandle) !=
                px::graphics::ScreenEffectStatus::Cancelled) {
            std::cerr << "FAIL: screen transition cancel lifecycle is incomplete\n";
            result = 1;
        }

        const auto fixtureRoot = std::filesystem::temp_directory_path() /
                                 "prismatix-visual-conformance";
        std::error_code fixtureError;
        std::filesystem::create_directories(fixtureRoot, fixtureError);
        const auto background = fixtureRoot / "background.bmp";
        const auto alternate = fixtureRoot / "alternate.bmp";
        const auto rule = fixtureRoot / "rule.bmp";
        if (fixtureError || !SaveFixtureSurface(background, false) ||
            !SaveFixtureSurface(alternate, true) ||
            !SaveFixtureSurface(rule, false, true)) {
            std::cerr << "FAIL: screen-effect visual fixtures could not be created\n";
            result = 1;
        } else {
            vfs.MountDirectory(fixtureRoot.string());
            const auto renderScreenEffect = [&](const std::string_view name,
                                                const double progress) {
                px::vn::Stage stage(renderer, assets);
                stage.SetBackground("background.bmp", false);
                if (name == "rule-dissolve") {
                    stage.SetBackgroundRule("alternate.bmp", "rule.bmp", 1000,
                                            48);
                }
                if (!stage.ApplyAnimationProperty("$camera", std::string(name),
                                                  px::Variant(progress))) {
                    return FrameEvidence{};
                }
                stage.Update(name == "shake" ? 0.016f : 0.0f);
                ClearFrame(device.Renderer());
                stage.Render();
                return CaptureFrame(device.Renderer(),
                                    "screen-" + std::string(name));
            };
            px::vn::Stage baselineStage(renderer, assets);
            baselineStage.SetBackground("background.bmp", false);
            baselineStage.Update(0.0f);
            ClearFrame(device.Renderer());
            baselineStage.Render();
            const FrameEvidence screenBaseline =
                CaptureFrame(device.Renderer(), "screen-baseline");
            std::set<std::uint64_t> screenHashes{screenBaseline.hash};
            for (const auto& [name, progress] :
                 std::initializer_list<std::pair<std::string_view, double>>{
                     {"fade", 0.55}, {"flash", 0.55}, {"shake", 0.55},
                     {"zoom", 0.72}, {"pan", 0.55}, {"blur", 0.75},
                     {"vignette", 0.85}, {"color-grade", 0.65},
                     {"rule-dissolve", 0.5}}) {
                const FrameEvidence evidence = renderScreenEffect(name, progress);
                if (evidence.hash == 0 || evidence.hash == screenBaseline.hash) {
                    std::cerr << "FAIL: public screen effect has no visual output: "
                              << name << '\n';
                    result = 1;
                }
                screenHashes.insert(evidence.hash);
            }
            if (screenHashes.size() != 10) {
                std::cerr << "FAIL: public screen effects are not visually distinct\n";
                result = 1;
            }
        }

        // Render every published text preset through the ordinary Galgame UI
        // tree. This catches property mutations that never reach Draw().
        vfs.MountDirectory(
            std::filesystem::path(PRISMATIX_RESOURCE_ROOT).parent_path().string());
        renderer.SetLogicalSize(1280, 720);
        const auto renderTextEffect = [&](const std::string_view effect) {
            px::ui::GalgameUI ui;
            ui.SetTextRenderer(&renderer);
            px::ui::DialoguePresentation presentation;
            presentation.speaker = "PrismatiX";
            presentation.text = "Effect 👩‍💻 é 漢字";
            if (!ui.ShowHUD(presentation)) return FrameEvidence{};
            px::Input input;
            (void)ui.Update(input, 1280, 720);
            if (!effect.empty()) {
                px::animation::TrackBinding binding{
                    .kind = px::animation::TargetKind::Text,
                    .target = "Dialogue",
                    .property = std::string(effect)};
                if (!ui.ApplyAnimationProperty(binding, px::Variant(0.43)))
                    return FrameEvidence{};
                // Offset-based effects participate in layout; exercise the
                // same next-frame arrange pass used by Player/Preview.
                (void)ui.Update(input, 1280, 720, 0.0f);
            }
            ClearFrame(device.Renderer());
            ui.Render(renderer);
            return CaptureFrame(device.Renderer(),
                                effect.empty() ? "text-baseline"
                                               : "text-" + std::string(effect));
        };
        const FrameEvidence textBaseline = renderTextEffect({});
        std::set<std::uint64_t> textHashes{textBaseline.hash};
        for (const std::string_view effect :
             {"typewriter", "fade", "slide", "pop", "shake", "wave",
              "rainbow", "glitch"}) {
            const FrameEvidence evidence = renderTextEffect(effect);
            if (evidence.hash == 0 || evidence.hash == textBaseline.hash) {
                std::cerr << "FAIL: public text effect has no rendered output: "
                          << effect << '\n';
                result = 1;
            }
            textHashes.insert(evidence.hash);
        }
        if (textHashes.size() != 9) {
            std::cerr << "FAIL: public text effects are not visually distinct\n";
            result = 1;
        }
    }
    device.Destroy();
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    if (result == 0)
        std::cout << "PASS: SDL_GPU compositor driver=" << driver << '\n';
    return result;
#endif
}
