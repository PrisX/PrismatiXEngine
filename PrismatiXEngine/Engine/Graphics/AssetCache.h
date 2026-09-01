#pragma once

#include "Engine/Graphics/Texture.h"
#include "Engine/IO/VFS.h"

#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_Renderer;
struct SDL_Surface;
struct TTF_Font;

namespace px::graphics {

enum class AssetPreloadPriority : std::uint8_t {
    Low,
    Normal,
    High,
    Critical,
};

using AssetPreloadBatchId = std::uint64_t;

struct AssetPreloadRequest {
    std::string path;
    AssetPreloadPriority priority = AssetPreloadPriority::Normal;
};

struct AssetPreloadFailure {
    std::string path;
    std::string code;
    std::string message;
};

struct AssetPreloadProgress {
    AssetPreloadBatchId batchId = 0;
    bool valid = false;
    std::size_t total = 0;
    std::size_t queued = 0;
    std::size_t running = 0;
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    std::size_t cancelled = 0;
    std::size_t decodedBytes = 0;
    std::vector<AssetPreloadFailure> failures;

    [[nodiscard]] std::size_t Completed() const {
        return succeeded + failed + cancelled;
    }
    [[nodiscard]] bool Finished() const {
        return valid && Completed() == total && queued == 0 && running == 0;
    }
};

class AssetCache {
public:
    AssetCache(SDL_Renderer* renderer, io::VFS& vfs);
    ~AssetCache();

    AssetCache(const AssetCache&) = delete;
    AssetCache& operator=(const AssetCache&) = delete;

    [[nodiscard]] TextureHandle Texture(const std::string& path);
    void PreloadTexture(
        const std::string& path,
        AssetPreloadPriority priority = AssetPreloadPriority::Normal);
    [[nodiscard]] AssetPreloadBatchId BeginTexturePreload(
        std::vector<AssetPreloadRequest> requests);
    bool CancelTexturePreload(AssetPreloadBatchId batchId);
    [[nodiscard]] AssetPreloadProgress TexturePreloadProgress(
        AssetPreloadBatchId batchId) const;
    // Starts a new content/session generation. Queued work is cancelled and
    // running decodes are allowed to finish, but their results can no longer
    // publish into the new session.
    void BeginAssetSession();
    [[nodiscard]] std::uint64_t AssetSession() const { return m_assetSession; }
    void SetPreloadConcurrency(std::size_t value);
    [[nodiscard]] std::size_t PreloadConcurrency() const {
        return m_preloadConcurrency;
    }
    void SetAsyncPreloadEnabled(bool enabled) { m_asyncPreloadEnabled = enabled; }
    [[nodiscard]] bool AsyncPreloadEnabled() const { return m_asyncPreloadEnabled; }
    void SetTextureBudget(std::size_t bytes) { m_textureBudgetBytes=bytes; }
    [[nodiscard]] std::size_t ResidentTextureBytes() const { return m_residentTextureBytes; }
    [[nodiscard]] bool HasResidentTexture(const std::string& path) const;

    // Call once per frame before any drawing: advances the LRU clock and evicts
    // textures that were not used recently (deferred so nothing in-flight on the
    // render queue is destroyed mid-frame).
    void BeginFrame();

    // Decodes an image into an uncached RGBA32 surface (rule transitions need
    // CPU pixel access). Caller destroys the surface.
    [[nodiscard]] SDL_Surface* LoadSurface(const std::string& path);

    // Decodes an in-memory image (e.g. a save thumbnail) and caches it under a
    // virtual key (convention: "mem://..."). Replaces any previous registration.
    TextureHandle RegisterMemoryTexture(const std::string& key,
                                        const void* data, std::size_t size);
    void UnregisterTexture(const std::string& key);

    [[nodiscard]] TTF_Font* Font(const std::string& path, int size, int outline = 0);
    // Locale switching replaces the ordered SDL_ttf fallback chain and drops
    // only font resources; textures and in-flight texture preloads survive.
    bool SetFontFallbackChain(std::vector<std::string> paths);
    [[nodiscard]] const std::vector<std::string>& FontFallbackChain() const {
        return m_fontFallbackChain;
    }

    static void TextureSize(TextureHandle texture, int& w, int& h);

    void Clear();

private:
    struct FontEntry {
        TTF_Font* font = nullptr;
        std::shared_ptr<io::Bytes> bytes;
    };

    struct TextureEntry {
        TextureResource texture;
        std::uint64_t lastUse = 0;
        std::size_t bytes = 0;
    };

    struct DecodeResult {
        SDL_Surface* surface = nullptr;
        int width = 0;
        int height = 0;
        std::size_t decodedBytes = 0;
        std::string error;
    };

    struct QueuedTexture {
        std::string path;
        AssetPreloadPriority priority = AssetPreloadPriority::Normal;
        std::uint64_t sequence = 0;
        std::uint64_t session = 0;
        bool cacheRequest = false;
        std::vector<AssetPreloadBatchId> batches;
    };

    struct PendingTexture {
        std::string path;
        std::uint64_t session = 0;
        bool cacheRequest = false;
        std::vector<AssetPreloadBatchId> batches;
        std::future<DecodeResult> future;
    };

    struct BatchState {
        AssetPreloadProgress progress;
        bool failureDiagnosticEmitted = false;
    };

    SDL_Renderer* m_renderer;
    io::VFS& m_vfs;
    std::unordered_map<std::string, TextureEntry> m_textures;
    std::unordered_map<std::string, QueuedTexture> m_queuedTextures;
    std::vector<PendingTexture> m_pendingTextures;
    std::unordered_map<AssetPreloadBatchId, BatchState> m_preloadBatches;
    std::unordered_map<std::string, FontEntry> m_fonts;
    std::vector<std::string> m_fontFallbackChain;
    bool m_configuringFontFallbacks = false;
    std::uint64_t m_frame = 0;
    std::uint64_t m_assetSession = 1;
    std::uint64_t m_nextPreloadBatch = 1;
    std::uint64_t m_nextPreloadSequence = 1;
    std::size_t m_textureBudgetBytes=512u*1024u*1024u;
    std::size_t m_residentTextureBytes=0;
    std::size_t m_preloadConcurrency = 4;
    bool m_asyncPreloadEnabled=true;

    void QueueTexture(const std::string& path, AssetPreloadPriority priority,
                      AssetPreloadBatchId batchId, bool cacheRequest);
    void LaunchQueuedTextures();
    void HarvestPendingTextures();
    void CompleteBatchRequest(AssetPreloadBatchId batchId,
                              const std::string& path, bool succeeded,
                              std::size_t decodedBytes,
                              std::string code = {},
                              std::string message = {});
    void EmitBatchFailureIfComplete(BatchState& batch);
    void DestroyResidentAssets();
    void DestroyFonts();
    void PruneBatchHistory();
};

}
