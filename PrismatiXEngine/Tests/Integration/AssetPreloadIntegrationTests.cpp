#include "Engine/Graphics/AssetCache.h"
#include "Engine/IO/VFS.h"
#include "Tests/TestSupport/TestHarness.h"

#include <SDL3/SDL.h>

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
    return suite.Finish();
}
