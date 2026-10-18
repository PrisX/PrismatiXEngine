#include "Engine/Animation/MotionRenderer.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Graphics/Screenshot.h"
#include "Engine/IO/VFS.h"
#include "Engine/Platform/Input.h"
#include "Engine/SDK/Motion.h"
#include "Engine/Text/Typography.h"
#include "Engine/UI/GalgameUI.h"
#include "Engine/UI/UiAdapter.h"
#include "Engine/UI/UITypeRegistry.h"
#include "Engine/UI/VirtualizedView.h"
#include "Engine/UI/Theme.h"
#include "Engine/UI/Widgets.h"
#include "Engine/VN/Runtime/Stage.h"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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
                        const bool alternate, const bool rule = false,
                        const bool opaque = false) {
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
    // An opaque fixture has no alpha channel at all, like an RGB PNG or JPEG.
    SDL_Surface* written = opaque ? SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGB24) : surface;
    const bool saved = written && SDL_SaveBMP(written, path.string().c_str());
    if (written && written != surface) SDL_DestroySurface(written);
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
        // Revision-2 atlas motion must change the sampled texture region,
        // not merely its pose metadata, at the authored step timestamp.
        {
            const auto atlasRoot = std::filesystem::temp_directory_path() /
                ("prismatix-motion-atlas-" + std::to_string(SDL_GetTicksNS()));
            std::error_code atlasError;
            std::filesystem::create_directories(atlasRoot / "Assets", atlasError);
            SDL_Surface* atlas = SDL_CreateSurface(2, 1, SDL_PIXELFORMAT_RGBA32);
            const bool saved = atlas &&
                SDL_WriteSurfacePixel(atlas, 0, 0, 245, 20, 25, 255) &&
                SDL_WriteSurfacePixel(atlas, 1, 0, 20, 35, 240, 255) &&
                SDL_SaveBMP(atlas, (atlasRoot / "Assets/atlas.bmp").string().c_str());
            if (atlas) SDL_DestroySurface(atlas);
            if (atlasError || !saved) {
                std::cerr << "FAIL: atlas motion texture fixture\n";
                result = 1;
            } else {
                px::io::VFS atlasVfs;
                atlasVfs.MountDirectory(atlasRoot.generic_string());
                px::graphics::AssetCache atlasAssets(device.Renderer(), atlasVfs);
                px::graphics::Renderer2D atlasRenderer(device.Renderer(), atlasAssets, true);
                atlasRenderer.SetLogicalSize(640, 360);
                const auto parsed = px::sdk::ParseMotion(R"({
                  "format":"PrismatiXMotion","schemaRevision":1,"id":"atlas",
                  "canvas":{"width":640,"height":360},"duration":1,"loop":false,
                  "layers":[{"id":"mark","image":"Assets/atlas.bmp",
                    "width":100,"height":100,"x":270,"y":130,"frames":[
                      {"x":0,"y":0,"width":1,"height":1},
                      {"x":1,"y":0,"width":1,"height":1}]}],
                  "tracks":[{"layer":"mark","property":"frame","keys":[
                    {"time":0,"value":0,"curve":"step"},
                    {"time":0.5,"value":1,"curve":"step"}]}]})");
                if (!parsed.Valid()) {
                    std::cerr << "FAIL: atlas motion renderer fixture parse\n";
                    result = 1;
                } else {
                    px::sdk::MotionSampler sampler(*parsed.clip);
                    ClearFrame(device.Renderer());
                    sampler.Seek(0.25);
                    px::animation::DrawMotionPose(atlasRenderer, *parsed.clip,
                                                  sampler, {0, 0, 640, 360});
                    const auto first = CaptureFrame(device.Renderer(), "motion-atlas-first").center;
                    ClearFrame(device.Renderer());
                    sampler.Seek(0.75);
                    px::animation::DrawMotionPose(atlasRenderer, *parsed.clip,
                                                  sampler, {0, 0, 640, 360});
                    const auto second = CaptureFrame(device.Renderer(), "motion-atlas-second").center;
                    if (first.r < 200 || first.b > 70 || second.b < 200 || second.r > 70) {
                        std::cerr << "FAIL: atlas motion frame regions did not change rendered pixels\n";
                        result = 1;
                    }
                    const auto externalPivot = px::sdk::ParseMotion(R"({
                      "format":"PrismatiXMotion","schemaRevision":1,"id":"external-pivot",
                      "canvas":{"width":640,"height":360},"duration":1,"loop":false,
                      "layers":[{"id":"mark","image":"Assets/atlas.bmp",
                        "width":60,"height":60,"x":300,"y":150,
                        "anchorX":0.5,"anchorY":-0.5,"frames":[
                          {"x":0,"y":0,"width":1,"height":1}]}],
                      "tracks":[]})");
                    if (!externalPivot.Valid()) {
                        std::cerr << "FAIL: revision-11 external pivot render fixture parse\n";
                        result = 1;
                    } else {
                        px::sdk::MotionSampler pivotSampler(*externalPivot.clip);
                        pivotSampler.Seek(0);
                        ClearFrame(device.Renderer());
                        px::animation::DrawMotionPose(atlasRenderer, *externalPivot.clip,
                                                      pivotSampler, {0, 0, 640, 360});
                        SDL_Surface* pixels = SDL_RenderReadPixels(
                            device.Renderer(), nullptr);
                        const auto above = ReadLogicalPixel(pixels, 300, 140, 640, 360);
                        const auto below = ReadLogicalPixel(pixels, 300, 200, 640, 360);
                        if (pixels) SDL_DestroySurface(pixels);
                        if (above.r > 40 || below.r < 180 || below.b > 70) {
                            std::cerr << "FAIL: external pivot did not move drawn image below anchor\n";
                            result = 1;
                        }
                    }
                    const auto nested = px::sdk::ParseMotion(R"({
                      "format":"PrismatiXMotion","schemaRevision":1,"id":"nested",
                      "canvas":{"width":640,"height":360},"duration":1,"loop":false,
                      "layers":[
                        {"id":"subclip","kind":"group","x":250,"y":130,
                         "timeOffset":-0.5,"timeScale":1},
                        {"id":"logo","parent":"subclip","image":"Assets/atlas.bmp",
                         "width":100,"height":100,"frames":[
                           {"x":0,"y":0,"width":1,"height":1}]}],
                      "tracks":[{"layer":"logo","property":"x","keys":[
                        {"time":0,"value":0,"curve":"linear"},
                        {"time":0.5,"value":100,"curve":"linear"}]}]})");
                    if (!nested.Valid()) {
                        std::cerr << "FAIL: revision-7 nested motion fixture parse\n";
                        result = 1;
                    } else {
                        px::sdk::MotionSampler nestedSampler(*nested.clip);
                        nestedSampler.Seek(0.5);
                        ClearFrame(device.Renderer());
                        px::animation::DrawMotionPose(atlasRenderer, *nested.clip,
                                                      nestedSampler, {0, 0, 640, 360});
                        SDL_Surface* pixels = SDL_RenderReadPixels(
                            device.Renderer(), nullptr);
                        const auto early = ReadLogicalPixel(pixels, 300, 180, 640, 360);
                        const auto future = ReadLogicalPixel(pixels, 400, 180, 640, 360);
                        if (pixels) SDL_DestroySurface(pixels);
                        nestedSampler.Seek(1);
                        ClearFrame(device.Renderer());
                        px::animation::DrawMotionPose(atlasRenderer, *nested.clip,
                                                      nestedSampler, {0, 0, 640, 360});
                        pixels = SDL_RenderReadPixels(device.Renderer(), nullptr);
                        const auto past = ReadLogicalPixel(pixels, 300, 180, 640, 360);
                        const auto late = ReadLogicalPixel(pixels, 400, 180, 640, 360);
                        if (pixels) SDL_DestroySurface(pixels);
                        if (early.r < 180 || future.r > 40 ||
                            past.r > 40 || late.r < 180) {
                            std::cerr << "FAIL: nested motion child-local time or group rendering\n";
                            result = 1;
                        }
                    }
                    const auto eased = px::sdk::ParseMotion(R"({
                      "format":"PrismatiXMotion","schemaRevision":1,"id":"eased",
                      "canvas":{"width":640,"height":360},"duration":1,"loop":false,
                      "layers":[{"id":"mark","image":"Assets/atlas.bmp",
                        "width":100,"height":100,"x":270,"y":130,
                        "frames":[{"x":0,"y":0,"width":1,"height":1}]}],
                      "tracks":[{"layer":"mark","property":"opacity","keys":[
                        {"time":0,"value":0,"curve":"cubicBezier",
                         "bezier":{"x1":0.1,"y1":0.8,"x2":0.4,"y2":0.9}},
                        {"time":1,"value":1,"curve":"linear"}]}]})");
                    if (!eased.Valid()) {
                        std::cerr << "FAIL: cubic motion render fixture parse\n";
                        result = 1;
                    } else {
                        px::sdk::MotionSampler easeSampler(*eased.clip);
                        easeSampler.Seek(0.5);
                        ClearFrame(device.Renderer());
                        px::animation::DrawMotionPose(atlasRenderer, *eased.clip,
                                                      easeSampler, {0, 0, 640, 360});
                        const auto easedPixel = CaptureFrame(
                            device.Renderer(), "motion-cubic-timing").center;
                        if (easedPixel.r < 180 || easedPixel.b > 70) {
                            std::cerr << "FAIL: cubic timing did not brighten the rendered midpoint\n";
                            result = 1;
                        }
                    }
                    auto blended = px::sdk::ParseMotion(R"({
                      "format":"PrismatiXMotion","schemaRevision":1,"id":"blend",
                      "canvas":{"width":640,"height":360},"duration":1,"loop":false,
                      "layers":[
                        {"id":"base","image":"Assets/atlas.bmp","width":100,
                         "height":100,"x":270,"y":130,"frames":[
                           {"x":0,"y":0,"width":1,"height":1}]},
                        {"id":"light","image":"Assets/atlas.bmp","width":100,
                         "height":100,"x":270,"y":130,"z":1,
                         "blendMode":"additive","frames":[
                           {"x":1,"y":0,"width":1,"height":1}]}],
                      "tracks":[]})");
                    if (!blended.Valid()) {
                        std::cerr << "FAIL: revision-4 blend motion fixture parse\n";
                        result = 1;
                    } else {
                        px::sdk::MotionSampler blendSampler(*blended.clip);
                        blendSampler.Seek(0);
                        ClearFrame(device.Renderer());
                        px::animation::DrawMotionPose(atlasRenderer, *blended.clip,
                                                      blendSampler, {0, 0, 640, 360});
                        const auto additive = CaptureFrame(
                            device.Renderer(), "motion-additive-layer").center;
                        if (additive.r < 220 || additive.b < 220) {
                            std::cerr << "FAIL: additive motion layer did not blend both colors\n";
                            result = 1;
                        }
                        blended.clip->layers[1].blendMode = px::sdk::MotionBlendMode::Multiply;
                        ClearFrame(device.Renderer());
                        px::animation::DrawMotionPose(atlasRenderer, *blended.clip,
                                                      blendSampler, {0, 0, 640, 360});
                        const auto multiply = CaptureFrame(
                            device.Renderer(), "motion-multiply-layer").center;
                        if (multiply.r > 80 || multiply.b > 80) {
                            std::cerr << "FAIL: multiply motion layer did not darken overlap\n";
                            result = 1;
                        }
                    }
                    const auto tinted = px::sdk::ParseMotion(R"({
                      "format":"PrismatiXMotion","schemaRevision":1,"id":"tinted",
                      "canvas":{"width":640,"height":360},"duration":1,"loop":false,
                      "layers":[
                        {"id":"group","kind":"group","tint":{"r":0.5,"g":1,"b":1}},
                        {"id":"mark","parent":"group","image":"Assets/atlas.bmp",
                         "width":100,"height":100,"x":270,"y":130,
                         "frames":[{"x":0,"y":0,"width":1,"height":1}]}],
                      "tracks":[{"layer":"mark","property":"tint","keys":[
                        {"time":0,"value":{"r":1,"g":1,"b":1},"curve":"linear"},
                        {"time":1,"value":{"r":0,"g":1,"b":1},"curve":"linear"}]}]})");
                    if (!tinted.Valid()) {
                        std::cerr << "FAIL: revision-8 tint motion fixture parse\n";
                        result = 1;
                    } else {
                        px::sdk::MotionSampler tintSampler(*tinted.clip);
                        ClearFrame(device.Renderer());
                        tintSampler.Seek(0);
                        px::animation::DrawMotionPose(atlasRenderer, *tinted.clip,
                                                      tintSampler, {0, 0, 640, 360});
                        const auto firstTint = CaptureFrame(
                            device.Renderer(), "motion-tint-first").center;
                        ClearFrame(device.Renderer());
                        tintSampler.Seek(1);
                        px::animation::DrawMotionPose(atlasRenderer, *tinted.clip,
                                                      tintSampler, {0, 0, 640, 360});
                        const auto finalTint = CaptureFrame(
                            device.Renderer(), "motion-tint-final").center;
                        if (firstTint.r < 100 || firstTint.r > 160 || finalTint.r > 30 ||
                            firstTint.b < 10 || finalTint.b < 10) {
                            std::cerr << "FAIL: inherited motion RGB tint did not modulate rendered pixels\n";
                            result = 1;
                        }
                    }
                    SDL_Surface* maskSurface = SDL_CreateSurface(
                        2, 1, SDL_PIXELFORMAT_RGBA32);
                    const bool maskSaved = maskSurface &&
                        SDL_WriteSurfacePixel(maskSurface, 0, 0, 255, 255, 255, 255) &&
                        SDL_WriteSurfacePixel(maskSurface, 1, 0, 255, 255, 255, 0) &&
                        SDL_SaveBMP(maskSurface,
                            (atlasRoot / "Assets/mask.bmp").string().c_str());
                    if (maskSurface) SDL_DestroySurface(maskSurface);
                    auto masked = px::sdk::ParseMotion(R"({
                      "format":"PrismatiXMotion","schemaRevision":1,"id":"masked",
                      "canvas":{"width":640,"height":360},"duration":1,"loop":false,
                      "layers":[
                        {"id":"content","image":"Assets/atlas.bmp","width":100,
                         "height":100,"x":270,"y":130,"mask":"coverage",
                         "frames":[{"x":0,"y":0,"width":1,"height":1}]},
                        {"id":"coverage","image":"Assets/mask.bmp","width":100,
                         "height":100,"x":270,"y":130}],
                      "tracks":[{"layer":"coverage","property":"x","keys":[
                        {"time":0,"value":270,"curve":"linear"},
                        {"time":1,"value":370,"curve":"linear"}]}]})");
                    if (!maskSaved || !masked.Valid()) {
                        std::cerr << "FAIL: revision-6 alpha mask fixture\n";
                        result = 1;
                    } else {
                        px::sdk::MotionSampler maskSampler(*masked.clip);
                        (void)atlasAssets.Texture("Assets/mask.bmp");
                        ClearFrame(device.Renderer());
                        px::animation::DrawMotionPose(atlasRenderer, *masked.clip,
                                                      maskSampler, {0, 0, 640, 360});
                        SDL_Surface* pixels = SDL_RenderReadPixels(device.Renderer(), nullptr);
                        const auto inside = ReadLogicalPixel(pixels, 290, 180, 640, 360);
                        const auto outside = ReadLogicalPixel(pixels, 350, 180, 640, 360);
                        if (pixels) SDL_DestroySurface(pixels);
                        if (inside.r < 180 || inside.b > 70 ||
                            outside.r > 40 || outside.b < 12) {
                            std::cerr << "FAIL: alpha mask did not clip content pixels\n";
                            result = 1;
                        }
                        SDL_Surface* halfSurface = SDL_CreateSurface(
                            2, 1, SDL_PIXELFORMAT_RGBA32);
                        const bool halfSaved = halfSurface &&
                            SDL_WriteSurfacePixel(halfSurface, 0, 0, 255, 255, 255, 128) &&
                            SDL_WriteSurfacePixel(halfSurface, 1, 0, 255, 255, 255, 0) &&
                            SDL_SaveBMP(halfSurface,
                                (atlasRoot / "Assets/half-mask.bmp").string().c_str());
                        if (halfSurface) SDL_DestroySurface(halfSurface);
                        if (halfSaved) {
                            masked.clip->layers[1].image = "Assets/half-mask.bmp";
                            (void)atlasAssets.Texture("Assets/half-mask.bmp");
                            ClearFrame(device.Renderer());
                            px::animation::DrawMotionPose(atlasRenderer, *masked.clip,
                                                          maskSampler, {0, 0, 640, 360});
                            pixels = SDL_RenderReadPixels(device.Renderer(), nullptr);
                            const auto half = ReadLogicalPixel(pixels, 290, 180, 640, 360);
                            if (pixels) SDL_DestroySurface(pixels);
                            if (half.r < 95 || half.r > 170) {
                                std::cerr << "FAIL: mask half-alpha was multiplied twice\n";
                                result = 1;
                            }
                            masked.clip->layers[1].image = "Assets/mask.bmp";
                        } else {
                            std::cerr << "FAIL: half-alpha mask fixture\n";
                            result = 1;
                        }
                        maskSampler.Seek(1);
                        ClearFrame(device.Renderer());
                        px::animation::DrawMotionPose(atlasRenderer, *masked.clip,
                                                      maskSampler, {0, 0, 640, 360});
                        pixels = SDL_RenderReadPixels(device.Renderer(), nullptr);
                        const auto movedContent = ReadLogicalPixel(
                            pixels, 290, 180, 640, 360);
                        const auto sourceOnly = ReadLogicalPixel(
                            pixels, 390, 180, 640, 360);
                        if (pixels) SDL_DestroySurface(pixels);
                        if (movedContent.r > 40 || sourceOnly.r > 40) {
                            std::cerr << "FAIL: animated mask or mask-only visibility\n";
                            result = 1;
                        }
                        SDL_Window* basicWindow = SDL_CreateWindow(
                            "PrismatiX basic mask", 640, 360, SDL_WINDOW_HIDDEN);
                        px::graphics::GraphicsDevice basicDevice;
                        if (!basicWindow || !basicDevice.Create(
                                basicWindow, px::graphics::GraphicsTier::Basic)) {
                            std::cerr << "FAIL: basic renderer mask fixture setup\n";
                            result = 1;
                        } else {
                            px::graphics::AssetCache basicAssets(
                                basicDevice.Renderer(), atlasVfs);
                            px::graphics::Renderer2D basicRenderer(
                                basicDevice.Renderer(), basicAssets, false);
                            basicRenderer.SetLogicalSize(640, 360);
                            (void)basicAssets.Texture("Assets/atlas.bmp");
                            (void)basicAssets.Texture("Assets/mask.bmp");
                            maskSampler.Seek(0);
                            ClearFrame(basicDevice.Renderer());
                            px::animation::DrawMotionPose(basicRenderer, *masked.clip,
                                                          maskSampler, {0, 0, 640, 360});
                            SDL_Surface* basicPixels = SDL_RenderReadPixels(
                                basicDevice.Renderer(), nullptr);
                            const auto basicInside = ReadLogicalPixel(
                                basicPixels, 290, 180, 640, 360);
                            const auto basicOutside = ReadLogicalPixel(
                                basicPixels, 350, 180, 640, 360);
                            if (basicPixels) SDL_DestroySurface(basicPixels);
                            if (basicInside.r < 180 || basicOutside.r > 40) {
                                std::cerr << "FAIL: basic renderer alpha mask parity\n";
                                result = 1;
                            }
                        }
                        basicDevice.Destroy();
                        if (basicWindow) SDL_DestroyWindow(basicWindow);
                    }
                }
            }
            std::filesystem::remove_all(atlasRoot, atlasError);
        }
        // A bound sprite alphabet must draw the selected images at authored
        // glyph offsets, rather than just exposing dependency metadata.
        {
            const auto spriteRoot = std::filesystem::temp_directory_path() /
                ("prismatix-sprite-text-" + std::to_string(SDL_GetTicksNS()));
            std::error_code spriteError;
            std::filesystem::create_directories(spriteRoot / "Assets", spriteError);
            const auto saveGlyph = [&](const char* name, const Pixel color) {
                SDL_Surface* surface = SDL_CreateSurface(1, 1, SDL_PIXELFORMAT_RGBA32);
                const bool saved = surface &&
                    SDL_WriteSurfacePixel(surface, 0, 0,
                        color.r, color.g, color.b, color.a) &&
                    SDL_SaveBMP(surface, (spriteRoot / "Assets" / name).string().c_str());
                if (surface) SDL_DestroySurface(surface);
                return saved;
            };
            if (spriteError || !saveGlyph("one.bmp", {240, 25, 30, 255}) ||
                !saveGlyph("dash.bmp", {25, 35, 240, 255})) {
                std::cerr << "FAIL: SpriteText image fixture\n";
                result = 1;
            } else {
                px::io::VFS spriteVfs;
                spriteVfs.MountDirectory(spriteRoot.generic_string());
                px::graphics::AssetCache spriteAssets(device.Renderer(), spriteVfs);
                px::graphics::Renderer2D spriteRenderer(device.Renderer(), spriteAssets, true);
                spriteRenderer.SetLogicalSize(640, 360);
                (void)spriteAssets.Texture("Assets/one.bmp");
                (void)spriteAssets.Texture("Assets/dash.bmp");
                px::ui::SpriteText sprite;
                sprite.SetGlyphs({{"1", "Assets/one.bmp", 280, 160, 20, 20},
                                  {"-", "Assets/dash.bmp", 280, 160, 20, 20}});
                sprite.SetAdvance(24);
                sprite.SetText("1-");
                sprite.Arrange({0, 0, 640, 360});
                px::ui::Theme spriteTheme;
                ClearFrame(device.Renderer());
                sprite.Draw(spriteRenderer, spriteTheme);
                SDL_Surface* frame = SDL_RenderReadPixels(device.Renderer(), nullptr);
                const Pixel first = ReadLogicalPixel(frame, 290, 170, 640, 360);
                const Pixel second = ReadLogicalPixel(frame, 314, 170, 640, 360);
                if (frame) SDL_DestroySurface(frame);
                if (first.r < 200 || first.b > 80 ||
                    second.b < 200 || second.r > 80) {
                    std::cerr << "FAIL: SpriteText did not paint distinct authored glyphs\n";
                    result = 1;
                }
            }
            std::filesystem::remove_all(spriteRoot, spriteError);
        }
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
        // Styled and ruby lines use the run layout, which keeps 禁則: the
        // full stop that would start the second line pulls a character
        // down with it instead.
        const std::string kinsokuText = "前前前前。後";
        const std::size_t stop = std::string("前前前前").size();
        const auto runLayout = renderer.LayoutTextRuns(
            kinsokuText, {{0, kinsokuText.size(), font, 24}}, 100);
        int stopLine = -1, beforeStopLine = -2;
        for (const auto& cluster : runLayout.Clusters()) {
            if (cluster.byteStart == stop) stopLine = cluster.line;
            if (cluster.byteStart + cluster.byteLength == stop) beforeStopLine = cluster.line;
        }
        if (stopLine <= 0 || stopLine != beforeStopLine) {
            std::cerr << "FAIL: the run layout started a line with a full stop (line "
                      << stopLine << ", previous " << beforeStopLine << ")\n";
            result = 1;
        }
        {
            // A wrapped ruby line draws its reading above the base.
            px::ui::RichTextLabel ruby("前[ruby=かんじ]漢字[/ruby]後");
            ruby.SetFontPath(font);
            ruby.SetFontSize(32);
            ruby.SetColor({20, 20, 20, 255});
            ruby.Arrange({40, 60, 300, 120});
            ClearFrame(device.Renderer());
            renderer.DrawRect({0, 0, 640, 360}, {255, 255, 255, 255});
            ruby.Draw(renderer, px::ui::Theme{});
            const auto withReading = CaptureFrame(device.Renderer(), "ruby-run-layout").hash;
            px::ui::RichTextLabel plain("前漢字後");
            plain.SetFontPath(font);
            plain.SetFontSize(32);
            plain.SetColor({20, 20, 20, 255});
            plain.Arrange({40, 60, 300, 120});
            ClearFrame(device.Renderer());
            renderer.DrawRect({0, 0, 640, 360}, {255, 255, 255, 255});
            plain.Draw(renderer, px::ui::Theme{});
            const auto withoutReading = CaptureFrame(device.Renderer(), "ruby-plain").hash;
            if (withReading == 0 || withReading == withoutReading) {
                std::cerr << "FAIL: a ruby line on the run layout drew no reading\n";
                result = 1;
            }
        }
        {
            // Control characters are line breaks or nothing, never a
            // missing-glyph box: "Aquatf\n" draws exactly as "Aquatf".
            const auto drawLabel = [&](const std::string& text, const std::string& name,
                                       const float spacing = 0.0f) {
                px::ui::Label label(text);
                label.SetFontPath(font);
                label.SetFontSize(40);
                label.SetColor({20, 20, 20, 255});
                label.SetLetterSpacing(spacing);
                label.SetHorizontalAlignment(px::ui::HorizontalTextAlignment::Center);
                label.Arrange({20, 20, 600, 320});
                ClearFrame(device.Renderer());
                renderer.DrawRect({0, 0, 640, 360}, {255, 255, 255, 255});
                label.Draw(renderer, px::ui::Theme{});
                return CaptureFrame(device.Renderer(), name).hash;
            };
            const auto plain = drawLabel("Aquatf", "control-plain");
            const auto trailing = drawLabel("Aquatf\n", "control-trailing-newline");
            const auto trailingCrLf = drawLabel("Aquatf\r\n", "control-trailing-crlf");
            const auto controls = drawLabel("Aqu\x01" "atf\x7f", "control-c0-del");
            const auto c1 = drawLabel("Aqu\xc2\x85" "atf", "control-c1");
            const auto spaced = drawLabel("Aquatf", "control-spaced", 4.0f);
            const auto spacedControl = drawLabel("Aquatf\x1b", "control-spaced-escape", 4.0f);
            if (plain == 0 || trailing != plain || trailingCrLf != plain || controls != plain ||
                c1 != plain || spacedControl != spaced) {
                std::cerr << "FAIL: a control character in UI text drew a glyph\n";
                result = 1;
            }
            const auto twoLines = drawLabel("Aqua\ntf", "control-two-lines");
            const auto twoLinesCrLf = drawLabel("Aqua\r\ntf", "control-two-lines-crlf");
            const auto twoLinesCr = drawLabel("Aqua\rtf", "control-two-lines-cr");
            if (twoLines == plain || twoLinesCrLf != twoLines || twoLinesCr != twoLines) {
                std::cerr << "FAIL: \\n, \\r\\n and \\r are not the same line break in UI text\n";
                result = 1;
            }
            if (drawLabel("Aqua\ttf", "control-tab") != drawLabel("Aqua tf", "control-space")) {
                std::cerr << "FAIL: a tab in UI text did not draw as a space\n";
                result = 1;
            }
            // Layout keeps the caller's byte offsets: the line after "\r\n"
            // starts at the byte after it, and controls take no width.
            const std::string crlfText = "AB\r\nCD";
            const auto crlfLayout = renderer.LayoutText(crlfText, font, 24);
            const auto lfLayout = renderer.LayoutText("AB\nCD", font, 24);
            int secondLine = -1;
            for (const auto& cluster : crlfLayout.Clusters()) {
                if (cluster.byteStart + cluster.byteLength > crlfText.size()) secondLine = -2;
                if (cluster.byteStart == 4 && secondLine != -2) secondLine = cluster.line;
            }
            if (secondLine != 1 || crlfLayout.Size().y != lfLayout.Size().y ||
                renderer.LayoutText("Aquatf\x01", font, 24).Size().x !=
                    renderer.LayoutText("Aquatf", font, 24).Size().x) {
                std::cerr << "FAIL: text layout measured a control character or misplaced a CRLF break\n";
                result = 1;
            }
            const auto controlRuns = renderer.LayoutTextRuns(
                crlfText, {{0, crlfText.size(), font, 24}}, 0);
            int runSecondLine = -1;
            for (const auto& cluster : controlRuns.Clusters())
                if (cluster.byteStart == 4) runSecondLine = cluster.line;
            if (runSecondLine != 1) {
                std::cerr << "FAIL: styled text did not break its line at \\r\\n\n";
                result = 1;
            }
            const auto mapped = px::text::MapDrawableText("a\r\nb\tc\x01" "d\re");
            if (mapped.text != "a\nb c" "d\ne" || mapped.sourceStart[1] != 1 ||
                mapped.sourceEnd[1] != 3 || mapped.sourceStart[5] != 7 ||
                px::text::HasControlCharacters("plain\n") ||
                !px::text::HasControlCharacters("tab\t")) {
                std::cerr << "FAIL: MapDrawableText did not normalise control characters\n";
                result = 1;
            }
        }
        {
            // Authored show animations: a delay hides the control, a left
            // wipe reveals it left to right, a stagger starts siblings one
            // after another, and list items play the view's item transition.
            const auto panel = [](const px::Color fill) {
                auto control = std::make_unique<px::ui::Control>();
                px::ui::BoxDecoration box;
                box.fill = fill;
                control->SetBoxDecoration(box);
                return control;
            };
            const auto frame = [&](px::ui::Control& root, const std::string& name) {
                ClearFrame(device.Renderer());
                renderer.DrawRect({0, 0, 640, 360}, {255, 255, 255, 255});
                root.Draw(renderer, px::ui::Theme{});
                SaveVisualArtifact(device.Renderer(), name);
                return SDL_RenderReadPixels(device.Renderer(), nullptr);
            };
            const auto red = [](SDL_Surface* surface, const int x, const int y) {
                const Pixel pixel = ReadLogicalPixel(surface, x, y, 640, 360);
                return pixel.r > 200 && pixel.g < 80 && pixel.b < 80;
            };
            const auto white = [](SDL_Surface* surface, const int x, const int y) {
                const Pixel pixel = ReadLogicalPixel(surface, x, y, 640, 360);
                return pixel.r > 240 && pixel.g > 240 && pixel.b > 240;
            };
            // Through the UI TypeRegistry, as authored runtimeProperties arrive.
            px::sdk::UiDocument document;
            document.id = "12000000-0000-4000-8000-000000000011";
            document.rootId = "12000000-0000-4000-8000-000000000012";
            px::sdk::UiNode node;
            node.id = document.rootId;
            node.kind = px::sdk::UiNodeKind::Control;
            node.runtimeType = "Control";
            node.runtimeProperties.emplace("showTransitionSeconds", 1.0);
            node.runtimeProperties.emplace("showDelaySeconds", 0.5);
            node.runtimeProperties.emplace("showWipe", std::string("left"));
            node.runtimeProperties.emplace("showFade", false);
            node.runtimeProperties.emplace("showOffsetY", 0.0);
            node.runtimeProperties.emplace("showTransitionEase", std::string("linear"));
            document.nodes.push_back(node);
            auto tree = px::ui::BuildUiRuntimeTree(document);
            if (!tree.Valid() || tree.root->ShowWipe() != px::ui::TransitionWipe::Left ||
                tree.root->ShowDelaySeconds() != 0.5f || tree.root->ShowFade()) {
                std::cerr << "FAIL: authored showWipe/showDelaySeconds/showFade were not applied\n";
                result = 1;
            } else {
                px::ui::BoxDecoration box;
                box.fill = {230, 30, 40, 255};
                tree.root->SetBoxDecoration(box);
                tree.root->Arrange({40, 40, 560, 280});
                tree.root->Update(0.25f);
                SDL_Surface* delayed = frame(*tree.root, "show-delay");
                const bool hidden = white(delayed, 320, 180);
                SDL_DestroySurface(delayed);
                tree.root->Update(0.75f);  // halfway through the 1s wipe
                SDL_Surface* wiping = frame(*tree.root, "show-wipe-left");
                const bool split = red(wiping, 120, 180) && white(wiping, 520, 180);
                SDL_DestroySurface(wiping);
                tree.root->Update(1.0f);
                SDL_Surface* done = frame(*tree.root, "show-wipe-done");
                const bool whole = red(done, 120, 180) && red(done, 580, 180);
                SDL_DestroySurface(done);
                if (!hidden || !split || !whole) {
                    std::cerr << "FAIL: delayed left wipe drew hidden=" << hidden
                              << " split=" << split << " whole=" << whole << '\n';
                    result = 1;
                }
            }
            // A staggering parent: the second child starts 0.5s after the first.
            px::ui::Control parent;
            parent.SetStaggerSeconds(0.5f);
            for (int index = 0; index < 2; ++index) {
                auto child = panel({230, 30, 40, 255});
                child->SetShowTransitionSeconds(0.1f);
                child->SetShowOffsetY(0.0f);
                (void)parent.AddChild(std::move(child));
            }
            parent.Arrange({0, 0, 640, 360});
            int slot = 0;
            for (const auto& child : parent.Children()) {
                auto* control = dynamic_cast<px::ui::Control*>(child.get());
                control->Arrange({40.0f + 300.0f * static_cast<float>(slot++), 40, 260, 280});
            }
            parent.Update(0.0f);
            parent.Update(0.3f);
            SDL_Surface* staggered = frame(parent, "show-stagger");
            const bool first = red(staggered, 170, 180);
            const bool secondWaits = white(staggered, 470, 180);
            SDL_DestroySurface(staggered);
            parent.Update(0.5f);
            SDL_Surface* both = frame(parent, "show-stagger-done");
            const bool second = red(both, 470, 180);
            SDL_DestroySurface(both);
            if (!first || !secondWaits || !second) {
                std::cerr << "FAIL: stagger first=" << first << " secondWaits=" << secondWaits
                          << " second=" << second << '\n';
                result = 1;
            }
            // Grid items get the view's item transition and stagger when it fills.
            struct Items final : px::ui::IVirtualItemSource {
                [[nodiscard]] std::size_t Count() const override { return 2; }
                [[nodiscard]] std::string StableKey(const std::size_t index) const override {
                    return std::to_string(index);
                }
                px::Status Bind(std::size_t, px::ui::Control&) override { return px::Status::Ok(); }
                [[nodiscard]] std::uint64_t Revision() const override { return 1; }
            };
            px::ui::GridView grid(2);
            grid.SetItemExtent({260, 280});
            grid.SetGap({40, 0});
            grid.SetStaggerSeconds(0.5f);
            auto& item = grid.EditItemShowTransition();
            item.seconds = 0.1f;
            item.offsetY = 0.0f;
            (void)grid.SetSource(std::make_shared<Items>(), [&] { return panel({230, 30, 40, 255}); });
            grid.Arrange({40, 40, 560, 280});
            grid.Update(0.0f);
            grid.Update(0.3f);
            SDL_Surface* items = frame(grid, "show-grid-items");
            const bool firstItem = red(items, 170, 180);
            const bool secondItemWaits = white(items, 470, 180);
            SDL_DestroySurface(items);
            if (!firstItem || !secondItemWaits) {
                std::cerr << "FAIL: grid item transition first=" << firstItem
                          << " secondWaits=" << secondItemWaits << '\n';
                result = 1;
            }
            // A show offset moves the control: halfway through a linear 1s
            // slide from 200px to the right it is drawn 100px right of its
            // place, and at rest it is back.
            {
                auto sliding = panel({230, 30, 40, 255});
                sliding->SetShowTransitionSeconds(1.0f);
                sliding->SetShowOffsetX(200.0f);
                sliding->SetShowOffsetY(0.0f);
                sliding->SetShowFade(false);
                sliding->SetShowEase(px::ui::TransitionEase::Linear);
                sliding->Arrange({40, 40, 260, 280});
                sliding->Update(0.0f);
                sliding->Update(0.5f);
                SDL_Surface* moving = frame(*sliding, "show-slide-half");
                const bool moved = white(moving, 120, 180) && red(moving, 380, 180);
                SDL_DestroySurface(moving);
                sliding->Update(0.6f);
                SDL_Surface* rest = frame(*sliding, "show-slide-rest");
                const bool settled = red(rest, 60, 180) && white(rest, 380, 180);
                SDL_DestroySurface(rest);
                // Scale grows about the centre and composes with the slide.
                sliding->SetShowScale(0.5f);
                sliding->RestartShowTransition();
                sliding->Update(0.0f);
                sliding->Update(0.5f);
                SDL_Surface* growing = frame(*sliding, "show-slide-scale-half");
                // At 0.5: scale 0.75 about the centre (170,180) is 195 wide,
                // then 100px right: x from 172.5 to 367.5.
                const bool composed = white(growing, 165, 180) && red(growing, 180, 180) &&
                    red(growing, 360, 180) && white(growing, 375, 180);
                SDL_DestroySurface(growing);
                if (!moved || !settled || !composed) {
                    std::cerr << "FAIL: show slide moved=" << moved << " settled=" << settled
                              << " composed=" << composed << '\n';
                    result = 1;
                }
                // A hide slides away: halfway through a linear hide towards
                // 200px down it is drawn 100px lower.
                sliding->SetShowScale(1.0f);
                sliding->FinishShowTransition();
                sliding->SetHideTransitionSeconds(1.0f);
                sliding->SetHideOffsetY(200.0f);
                sliding->SetHideFade(false);
                sliding->SetHideEase(px::ui::TransitionEase::Linear);
                SDL_DestroySurface(frame(*sliding, "hide-slide-before"));
                sliding->SetVisibility(px::ui::Visibility::Hidden);
                sliding->Update(0.5f);
                SDL_Surface* leaving = frame(*sliding, "hide-slide-half");
                const bool lower = white(leaving, 170, 100) && red(leaving, 170, 330);
                SDL_DestroySurface(leaving);
                if (!lower) {
                    std::cerr << "FAIL: hide slide did not move the control\n";
                    result = 1;
                }
                // `translation` moves the control as HitTest maps it.
                auto offsetPanel = panel({230, 30, 40, 255});
                offsetPanel->SetTranslation({200.0f, 0.0f});
                offsetPanel->Arrange({40, 40, 260, 280});
                SDL_Surface* translated = frame(*offsetPanel, "translation");
                const bool shifted = white(translated, 120, 180) && red(translated, 450, 180);
                SDL_DestroySurface(translated);
                if (!shifted || !offsetPanel->HitTest({450, 180}) || offsetPanel->HitTest({120, 180})) {
                    std::cerr << "FAIL: translation did not move the drawn control with its hit area\n";
                    result = 1;
                }
            }
            // showSound fires once as the show begins: after the parent's
            // delay and the control's own stagger, not again on re-layout, and
            // hideSound once as a shown control hides.
            {
                px::ui::Control screen;
                screen.SetShowDelaySeconds(1.5f);
                screen.SetStaggerSeconds(0.25f);
                std::vector<std::pair<std::string, float>> fired;
                float clock = 0.0f;
                const auto listen = [&](px::ui::Control& control, const std::string& name) {
                    (void)control.ConnectSignal("showSound", [&fired, &clock, name](const auto&) {
                        fired.emplace_back(name + ".show", clock);
                    });
                    (void)control.ConnectSignal("hideSound", [&fired, &clock, name](const auto&) {
                        fired.emplace_back(name + ".hide", clock);
                    });
                };
                listen(screen, "screen");
                std::vector<px::ui::Control*> rows;
                for (int index = 0; index < 3; ++index) {
                    auto row = std::make_unique<px::ui::Control>();
                    row->SetShowTransitionSeconds(0.3f);
                    row->SetShowDelaySeconds(index == 0 ? 0.0f : 1.6f);
                    rows.push_back(row.get());
                    (void)screen.AddChild(std::move(row));
                }
                listen(*rows[0], "first");
                listen(*rows[2], "third");
                screen.Arrange({0, 0, 640, 360});
                const auto run = [&](const float until) {
                    while (clock + 0.0001f < until) {
                        clock += 0.05f;
                        screen.Update(0.05f);
                    }
                };
                screen.Update(0.0f);
                run(1.4f);
                const bool quietDuringDelay = fired.empty();
                run(1.6f);
                screen.Arrange({0, 0, 640, 360});
                run(2.5f);
                // The screen and its first child start at 1.5 (the child
                // waits for its parent); the third waits 1.6 + 2 * 0.25.
                const auto at = [&](const std::string& name) {
                    for (const auto& [event, time] : fired) if (event == name) return time;
                    return -1.0f;
                };
                const auto count = [&](const std::string& name) {
                    return std::ranges::count_if(fired, [&](const auto& entry) { return entry.first == name; });
                };
                const bool timed = std::abs(at("screen.show") - 1.5f) < 0.06f &&
                    std::abs(at("first.show") - 1.5f) < 0.06f && std::abs(at("third.show") - 2.1f) < 0.06f;
                const bool once = count("screen.show") == 1 && count("first.show") == 1 && count("third.show") == 1;
                SDL_DestroySurface(frame(screen, "show-sound-shown"));
                rows[2]->SetVisibility(px::ui::Visibility::Hidden);
                run(2.6f);
                rows[2]->SetVisibility(px::ui::Visibility::Visible);
                // Shown again it waits its delay and stagger anew (2.6 + 2.1).
                run(5.0f);
                const bool hid = count("third.hide") == 1 && count("third.show") == 2;
                if (!quietDuringDelay || !timed || !once || !hid) {
                    std::cerr << "FAIL: show/hide signals quiet=" << quietDuringDelay << " timed=" << timed
                              << " once=" << once << " hid=" << hid << " screen=" << at("screen.show")
                              << " first=" << at("first.show") << " third=" << at("third.show") << '\n';
                    result = 1;
                }
            }
            // A show sound's own delay counts from the moment the control
            // began to show, and does not hold the control back: a screen
            // root shows at once and sounds 1.5s later.
            {
                px::ui::Control opening;
                opening.EditShowSound().path = "Assets/Audio/open.ogg";
                opening.EditShowSound().delaySeconds = 1.5f;
                float clock = 0.0f;
                float soundedAt = -1.0f;
                (void)opening.ConnectSignal("showSound", [&](const auto&) { soundedAt = clock; });
                opening.Arrange({0, 0, 640, 360});
                opening.Update(0.0f);
                const bool drawnAtOnce = opening.ShowStartedInTree();
                while (clock < 2.0f) {
                    clock += 0.05f;
                    opening.Update(0.05f);
                }
                if (!drawnAtOnce || std::abs(soundedAt - 1.5f) > 0.06f ||
                    std::abs(opening.VisibleSeconds() - 2.0f) > 0.06f) {
                    std::cerr << "FAIL: show sound delay drawnAtOnce=" << drawnAtOnce
                              << " soundedAt=" << soundedAt << '\n';
                    result = 1;
                }
            }
            // Authored showSound*/hideSound* runtime properties reach the control.
            {
                px::sdk::UiDocument sounding;
                sounding.id = "12000000-0000-4000-8000-000000000021";
                sounding.rootId = "12000000-0000-4000-8000-000000000022";
                px::sdk::UiNode root;
                root.id = sounding.rootId;
                root.kind = px::sdk::UiNodeKind::Control;
                root.runtimeType = "Control";
                root.runtimeProperties.emplace("showSoundPath", std::string("Assets/Audio/open.ogg"));
                root.runtimeProperties.emplace("showSoundVariants", px::sdk::UiArrayValue{
                    R"(["Assets/Audio/open-a.ogg","Assets/Audio/open-b.ogg"])"});
                root.runtimeProperties.emplace("showSoundVolume", 0.6);
                root.runtimeProperties.emplace("showSoundDelaySeconds", 1.5);
                root.runtimeProperties.emplace("hideSoundPath", std::string("Assets/Audio/close.ogg"));
                sounding.nodes.push_back(root);
                auto built = px::ui::BuildUiRuntimeTree(sounding);
                const bool applied = built.Valid() &&
                    built.root->ShowSound().path == "Assets/Audio/open.ogg" &&
                    built.root->ShowSound().variants.size() == 2 &&
                    std::abs(built.root->ShowSound().volume - 0.6f) < 0.001f &&
                    built.root->ShowSound().delaySeconds == 1.5f &&
                    built.root->HideSound().path == "Assets/Audio/close.ogg" &&
                    built.root->HideSound().volume == 1.0f;
                sounding.nodes[0].runtimeProperties["showSoundVolume"] = 1.5;
                const bool rejected = !px::ui::BuildUiRuntimeTree(sounding).Valid();
                if (!applied || !rejected) {
                    std::cerr << "FAIL: authored show/hide sounds applied=" << applied
                              << " rejectedLoudVolume=" << rejected << '\n';
                    result = 1;
                }
            }
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
        if (!renderer.BeginPresentationHold() ||
            !renderer.PresentationHoldActive() ||
            !renderer.BeginFrame({30, 75, 220, 255})) {
            std::cerr << "FAIL: transition-free route could not hold its outgoing frame\n";
            result = 1;
        } else {
            renderer.DrawRect({0, 0, 640, 360}, {30, 75, 220, 255});
            renderer.EndFrame();
            const FrameEvidence heldWithoutTransition =
                CaptureFrame(device.Renderer(), "route-none-preload-held");
            if (heldWithoutTransition.hash != outgoingFrame.hash) {
                std::cerr << "FAIL: route without a transition exposed its incoming frame before readiness\n";
                result = 1;
            }
            renderer.EndPresentationHold();
            if (renderer.PresentationHoldActive() ||
                !renderer.BeginFrame({30, 75, 220, 255})) {
                std::cerr << "FAIL: route presentation hold did not release\n";
                result = 1;
            } else {
                renderer.DrawRect({0, 0, 640, 360}, {30, 75, 220, 255});
                renderer.EndFrame();
                const FrameEvidence released =
                    CaptureFrame(device.Renderer(), "route-none-preload-ready");
                if (released.hash == outgoingFrame.hash) {
                    std::cerr << "FAIL: ready route did not appear without an animation\n";
                    result = 1;
                }
            }
            if (renderer.BeginFrame({220, 35, 45, 255})) {
                renderer.DrawRect({0, 0, 640, 360}, {220, 35, 45, 255});
                renderer.EndFrame();
            }
        }
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
        const auto opaqueBackground = fixtureRoot / "opaque-background.bmp";
        if (fixtureError || !SaveFixtureSurface(background, false) ||
            !SaveFixtureSurface(alternate, true) ||
            !SaveFixtureSurface(rule, false, true) ||
            !SaveFixtureSurface(opaqueBackground, false, false, true)) {
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

            // Halfway through a left-to-right rule, the left edge shows the
            // incoming image and the right edge still shows the outgoing one,
            // whichever of the GPU or CPU rule paths the device uses.
            // The outgoing background may have no alpha channel (an RGB PNG,
            // JPEG or WebP); the GPU rule mask must still see it as opaque.
            for (const std::string outgoingPath : {"background.bmp", "opaque-background.bmp"}) {
                if (!renderer.SupportsRuleMask()) {
                    std::cerr << "FAIL: gpu-effects renderer did not load the rule shader\n";
                    result = 1;
                }
                px::vn::Stage stage(renderer, assets);
                stage.SetBackground(outgoingPath, false);
                stage.SetBackgroundRule("alternate.bmp", "rule.bmp", 1000, 48);
                for (int frame = 0; frame < 500 &&
                     !assets.HasResidentTexture("alternate.bmp"); ++frame) {
                    assets.BeginFrame();
                    SDL_Delay(2);
                }
                (void)stage.ApplyAnimationProperty("$camera", "rule-dissolve",
                                                   px::Variant(0.5));
                stage.Update(0.0f);
                ClearFrame(device.Renderer());
                stage.Render();
                SaveVisualArtifact(device.Renderer(), "screen-rule-dissolve-split-" + outgoingPath);
                SDL_Surface* frame = SDL_RenderReadPixels(device.Renderer(), nullptr);
                const Pixel incoming = ReadLogicalPixel(frame, 64, 180, 640, 360);
                const Pixel outgoing = ReadLogicalPixel(frame, 576, 180, 640, 360);
                if (frame) SDL_DestroySurface(frame);
                const auto near = [](const Pixel& actual, const Pixel& expected) {
                    return std::abs(actual.r - expected.r) <= 12 &&
                           std::abs(actual.g - expected.g) <= 12 &&
                           std::abs(actual.b - expected.b) <= 12;
                };
                if (!near(incoming, {100, 99, 205, 255}) ||
                    !near(outgoing, {230, 246, 202, 255})) {
                    std::cerr << "FAIL: rule dissolve split from " << outgoingPath << " is wrong: incoming "
                              << int(incoming.r) << ',' << int(incoming.g) << ','
                              << int(incoming.b) << " outgoing " << int(outgoing.r)
                              << ',' << int(outgoing.g) << ',' << int(outgoing.b)
                              << " gpu=" << renderer.SupportsRuleMask() << '\n';
                    result = 1;
                }
            }
        }

        // Render every published text preset through the ordinary Galgame UI
        // tree. This catches property mutations that never reach Draw().
        vfs.MountDirectory(
            std::filesystem::path(PRISMATIX_RESOURCE_ROOT).parent_path().string());
        renderer.SetLogicalSize(1280, 720);
        // Optional project-level visual check. Render the compiled TSX scene
        // with the same native controls and textures used by Player.
        if (const char* configPath = SDL_getenv("PRISMATIX_TEST_CONFIG_UI");
            configPath && *configPath) {
            const char* projectRoot = SDL_getenv("PRISMATIX_TEST_PROJECT_ROOT");
            std::ifstream configSource(configPath, std::ios::binary);
            if (!projectRoot || !*projectRoot || !configSource) {
                std::cerr << "FAIL: external Config visual fixture paths\n";
                result = 1;
            } else {
                vfs.MountDirectory(projectRoot);
                const std::string document(
                    (std::istreambuf_iterator<char>(configSource)), {});
                renderer.SetLogicalSize(1920, 1080);
                for (const int page : {1, 3}) {
                    px::ui::GalgameUI configUi;
                    configUi.SetTextRenderer(&renderer);
                    bool ready = true;
                    for (int index = 1; index <= 4; ++index)
                        ready &= configUi.DefineUiData(
                            "settings.page" + std::to_string(index),
                            px::Variant(index == page)).IsOk();
                    ready &= configUi.RegisterTemplate(
                        px::ui::GalgameUI::Screen::Settings,
                        document, configPath).IsOk();
                    px::ui::SettingsPresentation settings;
                    settings.master = 55;
                    if (ready) ready = configUi.ShowSettings(settings).IsOk();
                    px::Input configInput;
                    if (ready) {
                        const std::string configBackground =
                            "Assets/Original/pc/ja/conf/bg0" +
                            std::to_string(page) + ".png";
                        renderer.PreloadImage(configBackground);
                        for (int attempt = 0; attempt < 100 &&
                             !renderer.ImageReady(configBackground); ++attempt) {
                            assets.BeginFrame();
                            SDL_Delay(10);
                        }
                        ready = renderer.ImageReady(configBackground);
                    }
                    if (ready) {
                        configInput.InjectFrame(page == 1 ? 930.0f : 1080.0f,
                                                page == 1 ? 220.0f : 222.0f,
                                                page == 3);
                        (void)configUi.Update(configInput, 1920, 1080);
                        ClearFrame(device.Renderer());
                        configUi.Render(renderer);
                        const auto name = page == 1
                            ? "sakura-config-radio" : "sakura-config-slider";
                        if (CaptureFrame(device.Renderer(), name).hash == 0)
                            ready = false;
                    }
                    if (!ready) {
                        std::cerr << "FAIL: external Config visual frame page="
                                  << page << '\n';
                        result = 1;
                    }
                }
                renderer.SetLogicalSize(1280, 720);
            }
        }
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
