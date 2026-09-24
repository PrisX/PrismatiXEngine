#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Graphics/Screenshot.h"
#include "Engine/IO/VFS.h"
#include "Engine/Platform/Input.h"
#include "Engine/Text/Typography.h"
#include "Engine/UI/GalgameUI.h"
#include "Engine/UI/UiAdapter.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Engine/UI/Theme.h"
#include "Engine/UI/Widgets.h"
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
        // A typewriter prefix can sit exactly at the wrapping boundary. The
        // coloured edge must use the fill's line layout, not SDL_ttf's wider
        // outlined-font metrics (which used to put the edge on line two).
        {
            const std::string glyphs = "天地玄黃宇宙洪荒";
            const std::string fontPath = "Fonts/NotoSansTC-Bold.ttf";
            TTF_Font* plain = assets.Font(fontPath, 48, 0);
            TTF_Font* stroked = assets.Font(fontPath, 48, 4);
            int plainWidth = 0, plainHeight = 0;
            int strokedWidth = 0, strokedHeight = 0;
            if (!plain || !stroked ||
                !TTF_GetStringSize(plain, glyphs.c_str(), glyphs.size(),
                                   &plainWidth, &plainHeight) ||
                !TTF_GetStringSize(stroked, glyphs.c_str(), glyphs.size(),
                                   &strokedWidth, &strokedHeight) ||
                strokedWidth <= plainWidth) {
                std::cerr << "FAIL: outlined text wrap fixture metrics\n";
                result = 1;
            } else {
                ClearFrame(device.Renderer());
                renderer.DrawTextInRect(glyphs,
                    {24, 24, static_cast<float>(plainWidth / 2 + 1), 120},
                    fontPath, 24, {255, 255, 255, 255},
                    px::graphics::HorizontalAlignment::Left,
                    px::graphics::VerticalAlignment::Top, true, 255, 2,
                    {255, 0, 0, 255});
                SDL_Surface* pixels = SDL_RenderReadPixels(device.Renderer(), nullptr);
                bool edgeBelowFirstLine = false;
                if (pixels) {
                    const int firstLineBottom = 24 + plainHeight / 2 + 4;
                    for (int y = firstLineBottom; y < 110 && !edgeBelowFirstLine; ++y)
                        for (int x = 24; x < 24 + plainWidth / 2 + 2; ++x) {
                            std::uint8_t r = 0, g = 0, b = 0, a = 0;
                            if (SDL_ReadSurfacePixel(pixels, x, y, &r, &g, &b, &a) &&
                                r > 90 && g < 70 && b < 70) {
                                edgeBelowFirstLine = true;
                                break;
                            }
                        }
                    SDL_DestroySurface(pixels);
                }
                if (!pixels || edgeBelowFirstLine) {
                    std::cerr << "FAIL: outlined typewriter text wrapped separately from fill\n";
                    result = 1;
                }
            }
        }
        // A packaged starter has only its own font path. Exercise typed static
        // captions through the adapter, not dynamic HUD SetText calls or a
        // checkout-only font fallback, and compare rendered pixels with blanks.
        const auto captionRoot = std::filesystem::temp_directory_path() /
            ("prismatix-caption-" + std::to_string(SDL_GetTicksNS()));
        std::error_code captionError;
        std::filesystem::create_directories(captionRoot / "Content/Fonts", captionError);
        std::filesystem::copy_file(
            std::filesystem::path(PRISMATIX_RESOURCE_ROOT) / "Fonts/NotoSansTC-Bold.ttf",
            captionRoot / "Content/Fonts/NotoSansTC-Bold.ttf",
            std::filesystem::copy_options::none, captionError);
        if (captionError || !px::ui::RegisterBuiltinUITypes()) {
            std::cerr << "FAIL: packaged caption font fixture: " << captionError.message() << '\n';
            result = 1;
        } else {
            px::io::VFS captionVfs;
            captionVfs.MountDirectory(captionRoot.generic_string());
            px::graphics::AssetCache captionAssets(device.Renderer(), captionVfs);
            px::graphics::Renderer2D captionRenderer(device.Renderer(), captionAssets, true);
            captionRenderer.SetLogicalSize(640, 360);
            const px::ui::Theme captionTheme;
            for (const std::string type : {"Label", "RichTextLabel", "Button", "IconButton"}) {
                const auto renderCaption = [&](const std::string& text) {
                    px::sdk::UiDocument document;
                    document.id = "12000000-0000-4000-8000-000000000001";
                    document.rootId = "12000000-0000-4000-8000-000000000002";
                    px::sdk::UiNode node;
                    node.id = document.rootId;
                    node.kind = type.find("Button") != std::string::npos
                                    ? px::sdk::UiNodeKind::Button : px::sdk::UiNodeKind::Label;
                    node.runtimeType = type;
                    node.text = text;
                    document.nodes.push_back(node);
                    auto tree = px::ui::BuildUiRuntimeTree(document);
                    if (!tree.Valid()) return std::uint64_t{0};
                    tree.root->Arrange({40, 40, 480, 100});
                    ClearFrame(device.Renderer());
                    tree.root->Draw(captionRenderer, captionTheme);
                    return CaptureFrame(device.Renderer(), "typed-caption-" + type +
                        (text.empty() ? "-blank" : "-text")).hash;
                };
                const auto blank = renderCaption("");
                const auto drawn = renderCaption("SAVE / 小光");
                if (blank == 0 || drawn == 0 || drawn == blank) {
                    std::cerr << "FAIL: authored " << type << " text did not change native pixels\n";
                    result = 1;
                }
            }
        }
        std::filesystem::remove_all(captionRoot, captionError);
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
        const std::string wrappedRubyText = "前前前前漢字後";
        const auto wrappedRuby = renderer.LayoutText(wrappedRubyText, font,
                                                     24, 54);
        const px::Rect wrappedRubyBounds = wrappedRuby.BoundsForRange(
            std::string("前前前前").size(), std::string("漢字").size());
        if (wrappedRubyBounds.w <= 0.0f || wrappedRubyBounds.h <= 0.0f ||
            wrappedRubyBounds.y <= 0.0f) {
            std::cerr << "FAIL: wrapped ruby base does not retain its shaped line geometry\n";
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
        const auto heldHandle = renderer.PlayScreenEffect("crossfade", 1.0f);
        if (!heldHandle || !renderer.PauseScreenEffect(heldHandle, true)) {
            std::cerr << "FAIL: screen transition cannot be held for asset readiness\n";
            result = 1;
        }
        renderer.UpdateScreenEffects(0.75f);
        const auto held = renderer.ActiveScreenEffect();
        if (!held || !held->paused || held->elapsedSeconds != 0.0f ||
            !renderer.ScreenEffectPlaying(heldHandle) ||
            !renderer.PauseScreenEffect(heldHandle, false)) {
            std::cerr << "FAIL: held transition advanced or lost its playback state\n";
            result = 1;
        }
        renderer.UpdateScreenEffects(0.25f);
        const auto resumed = renderer.ActiveScreenEffect();
        if (!resumed || resumed->paused ||
            std::abs(resumed->elapsedSeconds - 0.25f) > 0.001f) {
            std::cerr << "FAIL: held transition did not resume from its original time\n";
            result = 1;
        }
        renderer.UpdateScreenEffects(0.75f);
        const auto crossfadeHandle = renderer.PlayScreenEffect("crossfade", 1.0f);
        renderer.UpdateScreenEffects(0.5f);
        FrameEvidence midpoint;
        if (!crossfadeHandle || !renderer.BeginFrame({220, 35, 45, 255})) {
            std::cerr << "FAIL: crossfade midpoint could not start\n";
            result = 1;
        } else {
            renderer.DrawRect({0, 0, 640, 360}, {220, 35, 45, 255});
            renderer.EndFrame();
            midpoint = CaptureFrame(device.Renderer(),
                                    "transition-crossfade-midpoint");
            if (std::abs(static_cast<int>(midpoint.center.r) - 125) > 12 ||
                std::abs(static_cast<int>(midpoint.center.g) - 55) > 12 ||
                std::abs(static_cast<int>(midpoint.center.b) - 133) > 12) {
                std::cerr << "FAIL: crossfade midpoint must mix both frames without a dark dip\n";
                result = 1;
            }
        }
        const auto restartedHandle = renderer.PlayScreenEffect("crossfade", 1.0f);
        if (!restartedHandle || renderer.ScreenEffectState(crossfadeHandle) !=
                px::graphics::ScreenEffectStatus::Cancelled ||
            !renderer.BeginFrame({20, 180, 60, 255})) {
            std::cerr << "FAIL: crossfade could not restart from the composited frame\n";
            result = 1;
        } else {
            renderer.DrawRect({0, 0, 640, 360}, {20, 180, 60, 255});
            renderer.EndFrame();
            const FrameEvidence restarted =
                CaptureFrame(device.Renderer(), "transition-crossfade-restarted");
            if (std::abs(static_cast<int>(restarted.center.r) -
                         static_cast<int>(midpoint.center.r)) > 5 ||
                std::abs(static_cast<int>(restarted.center.g) -
                         static_cast<int>(midpoint.center.g)) > 5 ||
                std::abs(static_cast<int>(restarted.center.b) -
                         static_cast<int>(midpoint.center.b)) > 5) {
                std::cerr << "FAIL: rapid route transition must begin at the visible composite\n";
                result = 1;
            }
        }
        renderer.UpdateScreenEffects(1.0f);
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
        {
            px::ui::GalgameUI slotsUi;
            slotsUi.SetTextRenderer(&renderer);
            std::vector<px::ui::GalgameItem> slots;
            for (int index = 0; index < 4; ++index)
                slots.push_back({std::to_string(index), "Slot " + std::to_string(index + 1),
                    "2026-09-22 12:34 / 小光\nA deliberately long saved dialogue preview that must stay inside this card",
                    "", false, "save.slot", std::to_string(index)});
            if (!slotsUi.ShowSaveLoad(false, slots)) result = 1;
            px::Input slotsInput;
            (void)slotsUi.Update(slotsInput, 1280, 720);
            std::vector<const px::scene::Node*> pending{slotsUi.Root()};
            int count = 0;
            while (!pending.empty()) {
                const auto* node = pending.back(); pending.pop_back();
                if (const auto* button = dynamic_cast<const px::ui::Button*>(node);
                    button && button->Text().find("Slot ") != std::string::npos) {
                    ++count;
                    const auto rect = button->LayoutRect();
                    if (rect.x < 50 || rect.x + rect.w > 1230.1f ||
                        button->Text().find('\n') != std::string::npos ||
                        button->AccessibilityDescription().find("小光") == std::string::npos) {
                        std::cerr << "FAIL: save card bounds or accessible caption\n"; result = 1;
                    }
                }
                for (const auto& child : node->Children()) pending.push_back(child.get());
            }
            if (count != 4) { std::cerr << "FAIL: save card count\n"; result = 1; }
            ClearFrame(device.Renderer());
            slotsUi.Render(renderer);
            const auto metadataPixels = CaptureFrame(device.Renderer(), "save-card-metadata").hash;
            for (auto& slot : slots) slot.subtitle.clear();
            (void)slotsUi.ShowSaveLoad(false, slots);
            (void)slotsUi.Update(slotsInput, 1280, 720);
            ClearFrame(device.Renderer());
            slotsUi.Render(renderer);
            const auto titlePixels = CaptureFrame(device.Renderer(), "save-card-title-only").hash;
            if (metadataPixels == 0 || titlePixels == 0 || metadataPixels == titlePixels) {
                std::cerr << "FAIL: save metadata must produce visible pixels\n"; result = 1;
            }
        }
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
