#include "Engine/Graphics/AssetCache.h"
#include "Engine/IO/VFS.h"
#include "Tests/TestSupport/TestHarness.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

bool WriteBitmap(const std::filesystem::path& path, const int width,
                 const int height, const std::uint32_t color) {
    SDL_Surface* surface =
        SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return false;
    const bool filled = SDL_FillSurfaceRect(surface, nullptr, color);
    const bool saved = filled && SDL_SaveBMP(surface, path.string().c_str());
    SDL_DestroySurface(surface);
    return saved;
}

px::graphics::AssetPreloadProgress WaitForProgress(
    px::graphics::AssetCache& assets,
    const px::graphics::AssetPreloadBatchId batch,
    const std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    px::graphics::AssetPreloadProgress progress;
    do {
        assets.BeginFrame();
        progress = assets.TexturePreloadProgress(batch);
        if (progress.Finished()) return progress;
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);
    assets.BeginFrame();
    return assets.TexturePreloadProgress(batch);
}


bool WriteNoisePng(const std::filesystem::path& path, const int width,
                   const int height, std::uint32_t seed) {
    SDL_Surface* surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return false;
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(
            static_cast<std::uint8_t*>(surface->pixels) + y * surface->pitch);
        for (int x = 0; x < width; ++x) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            row[x] = seed | 0xff000000u;
        }
    }
    const bool saved = IMG_SavePNG(surface, path.string().c_str());
    SDL_DestroySurface(surface);
    return saved;
}

double Percentile(std::vector<double> values, const double fraction) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>((values.size() - 1) * fraction)];
}

void MeasureConcurrentPng(px::test::Suite& suite) {
    suite.Require(SDL_Init(SDL_INIT_VIDEO), "benchmark SDL video initializes");
    px::test::TempDirectory fixture("asset-preload-benchmark");
    std::vector<px::graphics::AssetPreloadRequest> requests;
    nlohmann::json files = nlohmann::json::array();
    std::uint64_t sourceBytes = 0;
    for (int i = 0; i < 16; ++i) {
        const int width = 768 + (i % 4) * 128;
        const int height = 512 + (i / 4) * 128;
        const std::string path = "noise-" + std::to_string(i) + ".png";
        suite.Require(WriteNoisePng(fixture.path / path, width, height,
                                    20260922u + static_cast<std::uint32_t>(i)),
                      "real independently seeded PNG fixture is written");
        const auto bytes = std::filesystem::file_size(fixture.path / path);
        sourceBytes += bytes;
        files.push_back({{"path", path}, {"width", width}, {"height", height},
                         {"bytes", bytes}, {"codec", "PNG RGBA8"}});
        requests.push_back({.path = path});
    }
    SDL_Window* window = SDL_CreateWindow("Asset preload measurement", 320, 180,
                                           SDL_WINDOW_HIDDEN);
    suite.Require(window != nullptr, "benchmark hidden window exists");
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    suite.Require(renderer != nullptr, "benchmark native test renderer exists");
    (void)SDL_SetRenderVSync(renderer, 0);
    px::io::VFS vfs;
    vfs.MountDirectory(fixture.path.string());
    nlohmann::json cases = nlohmann::json::array();
    {
        px::graphics::AssetCache assets(renderer, vfs);
        assets.SetPreloadConcurrency(16);
        if (SDL_getenv("PRISMATIX_ASSET_BENCHMARK_UNBOUNDED"))
            assets.SetPreloadFrameLimits(32, 32);
        for (const bool warm : {false, true}) {
            std::vector<double> beginTimes, frameTimes, activeBeginTimes, completionTimes;
            std::size_t maximumCompletions = 0;
            std::vector<std::size_t> completions;
            for (int round = 0; round < 8; ++round) {
                if (!warm) assets.BeginAssetSession();
                const auto batch = assets.BeginTexturePreload(requests);
                std::size_t previous = assets.TexturePreloadProgress(batch).succeeded;
                int finishedFrames = 0;
                const auto roundStart = std::chrono::steady_clock::now();
                const auto deadline = std::chrono::steady_clock::now() + 10s;
                for (int frame = 0; finishedFrames < 16; ++frame) {
                    const auto queuedBefore = assets.TexturePreloadProgress(batch).queued;
                    const auto start = std::chrono::steady_clock::now();
                    assets.BeginFrame();
                    const auto harvested = std::chrono::steady_clock::now();
                    (void)SDL_RenderClear(renderer);
                    for (const auto& request : requests) {
                        if (!assets.HasResidentTexture(request.path)) continue;
                        const auto texture = assets.Texture(request.path);
                        (void)SDL_RenderTexture(renderer, static_cast<SDL_Texture*>(
                            texture.Native(px::graphics::TextureBackend::SdlRenderer)), nullptr, nullptr);
                    }
                    (void)SDL_RenderPresent(renderer);
                    const auto end = std::chrono::steady_clock::now();
                    beginTimes.push_back(std::chrono::duration<double, std::milli>(harvested - start).count());
                    frameTimes.push_back(std::chrono::duration<double, std::milli>(end - start).count());
                    const auto progress = assets.TexturePreloadProgress(batch);
                    const auto delta = progress.succeeded - previous;
                    completions.push_back(delta);
                    maximumCompletions = std::max(maximumCompletions, delta);
                    previous = progress.succeeded;
                    if (delta > 0 || progress.queued < queuedBefore)
                        activeBeginTimes.push_back(beginTimes.back());
                    if (progress.Finished()) {
                        if (finishedFrames == 0)
                            completionTimes.push_back(std::chrono::duration<double, std::milli>(end - roundStart).count());
                        ++finishedFrames;
                    }
                    suite.Require(std::chrono::steady_clock::now() < deadline,
                                  "benchmark preload finishes within ten seconds");
                    // Deliberately let concurrent decodes become ready together.
                    // This controlled wait is outside measured BeginFrame/frame time.
                    if (!warm && frame == 0) std::this_thread::sleep_for(150ms);
                }
                const auto progress = assets.TexturePreloadProgress(batch);
                suite.Expect(progress.Finished() && progress.succeeded == requests.size() && progress.failed == 0,
                             "every valid PNG decodes and uploads in every round");
            }
            cases.push_back({{"condition", warm ? "resident-warm" : "resident-cold-decode-burst"},
                {"samples", beginTimes.size()}, {"activeSamples", activeBeginTimes.size()},
                {"activeBeginFrameP95Ms", Percentile(activeBeginTimes, 0.95)}, {"activeBeginFrameP99Ms", Percentile(activeBeginTimes, 0.99)},
                {"completionWallTimesMsIncludingRendezvous", completionTimes}, {"beginFrameP95Ms", Percentile(beginTimes, 0.95)},
                {"beginFrameP99Ms", Percentile(beginTimes, 0.99)}, {"beginFrameMaxMs", Percentile(beginTimes, 1)},
                {"frameP95Ms", Percentile(frameTimes, 0.95)}, {"frameP99Ms", Percentile(frameTimes, 0.99)},
                {"frameMaxMs", Percentile(frameTimes, 1)}, {"maximumCompletionsPerFrame", maximumCompletions},
                {"beginFrameSamplesMs", beginTimes}, {"frameSamplesMs", frameTimes}, {"completionsPerFrame", completions},
                {"residentTextureBytes", assets.ResidentTextureBytes()}});
        }
    }
    nlohmann::json report = {{"kind", "asset-preload-native-test-renderer"}, {"seed", 20260922},
        {"renderer", SDL_GetRendererName(renderer)},
        {"frameAdmission", SDL_getenv("PRISMATIX_ASSET_BENCHMARK_UNBOUNDED") ? "unbounded-control-32" : "bounded-default-2"}, {"sourceBytes", sourceBytes}, {"files", files}, {"cases", cases},
        {"limitations", {"Resident cold does not mean OS disk-cache cold", "Hidden SDL test renderer, not a full Player/GPU workload", "150ms controlled decode rendezvous excluded from frame samples"}}};
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        report["processPeakWorkingSetBytes"] = counters.PeakWorkingSetSize;
#endif
    std::cout << report.dump() << '\n';
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

}  // namespace

int main() {
    px::test::Suite suite("AssetPreloadIntegration");
    suite.Run("PriorityFailureProgressCancellationAndBudget", [&] {
        suite.Require(SDL_Init(SDL_INIT_VIDEO), "SDL video initializes");
        px::test::TempDirectory fixture("asset-preload");
        suite.Require(WriteBitmap(fixture.path / "critical.bmp", 48, 24,
                                  0xff3366ffu) &&
                          WriteBitmap(fixture.path / "stale.bmp", 64, 32,
                                      0x33cc88ffu),
                      "texture fixtures are written");
        {
            std::ofstream invalid(fixture.path / "invalid.bin",
                                  std::ios::binary | std::ios::trunc);
            invalid << "not an image";
        }

        SDL_Window* window = SDL_CreateWindow(
            "PrismatiX asset preload", 160, 90, SDL_WINDOW_HIDDEN);
        suite.Require(window != nullptr, "hidden native window is created");
        SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
        suite.Require(renderer != nullptr, "native renderer is created");

        px::io::VFS vfs;
        vfs.MountDirectory(fixture.path.string());
        {
            px::graphics::AssetCache assets(renderer, vfs);
            assets.SetPreloadConcurrency(1);
            const auto batch = assets.BeginTexturePreload({
                {.path = "invalid.bin",
                 .priority = px::graphics::AssetPreloadPriority::Low},
                {.path = "critical.bmp",
                 .priority = px::graphics::AssetPreloadPriority::Critical},
            });
            assets.BeginFrame();
            px::graphics::AssetPreloadProgress mid;
            const auto midDeadline = std::chrono::steady_clock::now() + 2s;
            do {
                std::this_thread::sleep_for(5ms);
                assets.BeginFrame();
                mid = assets.TexturePreloadProgress(batch);
            } while (mid.succeeded == 0 &&
                     std::chrono::steady_clock::now() < midDeadline);
            suite.Expect(mid.succeeded == 1 && mid.failed == 0 &&
                             mid.running == 1,
                         "critical texture completes before the queued low-priority failure");

            const auto completed = WaitForProgress(assets, batch);
            suite.Expect(completed.Finished() && completed.total == 2 &&
                             completed.succeeded == 1 && completed.failed == 1 &&
                             completed.failures.size() == 1 &&
                             completed.failures.front().path == "invalid.bin" &&
                             completed.decodedBytes == 48u * 24u * 4u,
                         "batch exposes deterministic progress and aggregated failures");
            suite.Expect(assets.HasResidentTexture("critical.bmp"),
                         "successful preload publishes a renderer texture at a frame boundary");
            const auto criticalTexture = assets.Texture("critical.bmp");
            suite.Expect(criticalTexture &&
                             criticalTexture.Backend() ==
                                 px::graphics::TextureBackend::SdlRenderer &&
                             criticalTexture.Width() == 48 &&
                             criticalTexture.Height() == 24 &&
                             criticalTexture.Generation() == assets.AssetSession(),
                         "asset consumers receive backend-neutral identity, dimensions, and generation");

            const auto staleBatch = assets.BeginTexturePreload({{
                .path = "stale.bmp",
                .priority = px::graphics::AssetPreloadPriority::Critical,
            }});
            assets.BeginFrame();
            const std::uint64_t previousSession = assets.AssetSession();
            assets.BeginAssetSession();
            const auto cancelled = assets.TexturePreloadProgress(staleBatch);
            suite.Expect(assets.AssetSession() == previousSession + 1 &&
                             cancelled.Finished() && cancelled.cancelled == 1,
                         "content-session switch cancels the running batch immediately");
            for (int attempt = 0; attempt < 20; ++attempt) {
                std::this_thread::sleep_for(5ms);
                assets.BeginFrame();
            }
            suite.Expect(!assets.HasResidentTexture("stale.bmp") &&
                             !assets.HasResidentTexture("critical.bmp"),
                         "old-session decode and resident textures cannot write into the new session");

            const auto currentBatch = assets.BeginTexturePreload({{
                .path = "stale.bmp",
                .priority = px::graphics::AssetPreloadPriority::High,
            }});
            const auto current = WaitForProgress(assets, currentBatch);
            suite.Require(current.Finished() && current.succeeded == 1 &&
                              assets.HasResidentTexture("stale.bmp"),
                          "new-session preload succeeds after stale work is discarded");
            assets.SetTextureBudget(1);
            assets.BeginFrame();
            assets.BeginFrame();
            suite.Expect(!assets.HasResidentTexture("stale.bmp") &&
                             assets.ResidentTextureBytes() == 0,
                         "LRU enforcement returns resident texture usage to the configured budget");
        }

        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    });

    suite.Run("FrameAdmissionAndImageLimits", [&] {
        suite.Require(SDL_Init(SDL_INIT_VIDEO), "limit test video initializes");
        px::test::TempDirectory fixture("asset-preload-limits");
        std::vector<px::graphics::AssetPreloadRequest> requests;
        for (int i = 0; i < 8; ++i) {
            const auto path = "valid-" + std::to_string(i) + ".png";
            suite.Require(WriteNoisePng(fixture.path / path, 128 + i, 96 + i,
                                        17u + static_cast<std::uint32_t>(i)),
                          "bounded scheduling fixture is valid PNG");
            requests.push_back({.path = path});
        }
        suite.Require(WriteNoisePng(fixture.path / "large.png", 512, 512, 19u),
                      "decode limit fixture is a valid PNG with one MiB decoded pixels");
        {
            std::ofstream oversized(fixture.path / "oversized.png", std::ios::binary);
            oversized.seekp(128u * 1024u * 1024u);
            oversized.put('\0');
        }
        SDL_Window* window = SDL_CreateWindow("Asset limits", 160, 90, SDL_WINDOW_HIDDEN);
        suite.Require(window != nullptr, "limit test hidden window exists");
        SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
        suite.Require(renderer != nullptr, "limit test renderer exists");
        px::io::VFS vfs;
        vfs.MountDirectory(fixture.path.string());
        {
            px::graphics::AssetCache assets(renderer, vfs);
            assets.SetPreloadConcurrency(8);
            assets.SetPreloadFrameLimits(3, 1);
            const auto batch = assets.BeginTexturePreload(requests);
            assets.BeginFrame();
            const auto started = assets.TexturePreloadProgress(batch);
            suite.Expect(started.running == 3 && started.queued == 5,
                         "frame start admission is independent of total concurrency");
            std::size_t completed = 0;
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            do {
                std::this_thread::sleep_for(2ms);
                assets.BeginFrame();
                const auto progress = assets.TexturePreloadProgress(batch);
                suite.Expect(progress.succeeded - completed <= 1,
                             "ready decoded textures obey the per-frame upload limit");
                completed = progress.succeeded;
                if (progress.Finished()) break;
            } while (std::chrono::steady_clock::now() < deadline);
            suite.Expect(completed == requests.size(), "admission limits preserve eventual completion");
            assets.SetImageDecodeByteLimit(128u * 1024u);
            const auto rejected = assets.BeginTexturePreload({{.path = "large.png"}, {.path = "oversized.png"}});
            const auto progress = WaitForProgress(assets, rejected);
            suite.Expect(progress.Finished() && progress.failed == 2 && progress.succeeded == 0,
                         "encoded and decoded limits reject oversized inputs without publication");
            suite.Expect(progress.failures.size() == 2 &&
                std::any_of(progress.failures.begin(), progress.failures.end(), [](const auto& failure) {
                    return failure.message.find("PNG dimensions exceed") != std::string::npos;
                }), "PNG dimensions are rejected before decoder allocation with a useful diagnostic");
            suite.Expect(!assets.Texture("large.png"), "synchronous texture uses the same decoded limit");
            SDL_Surface* surface = assets.LoadSurface("large.png");
            suite.Expect(surface == nullptr, "surface decode uses the same decoded limit");
            if (surface) SDL_DestroySurface(surface);
            const auto bytes = vfs.Read("large.png");
            suite.Require(bytes.has_value(), "valid PNG bytes are available for memory test");
            suite.Expect(!assets.RegisterMemoryTexture("mem://limited", bytes->data(), bytes->size()),
                         "memory textures cannot bypass decoded image limits");
            assets.SetImageDecodeByteLimit(4u * 1024u * 1024u);
            const auto retried = assets.BeginTexturePreload({{.path = "large.png"}});
            suite.Expect(WaitForProgress(assets, retried).succeeded == 1,
                         "a valid image can be retried after an explicit limit adjustment");
        }
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    });
    if (SDL_getenv("PRISMATIX_ASSET_BENCHMARK"))
        suite.Run("MeasuredConcurrentPng", [&] { MeasureConcurrentPng(suite); });
    return suite.Finish();
}
