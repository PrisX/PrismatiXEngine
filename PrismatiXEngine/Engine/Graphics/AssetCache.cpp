#include "Engine/Graphics/AssetCache.h"

#include "Engine/Support/Logger.h"
#include "Engine/Diagnostics/Diagnostic.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <cstring>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

namespace px::graphics {

namespace {
constexpr std::size_t kMaximumEncodedImageBytes = 128u * 1024u * 1024u;

std::optional<io::Bytes> ReadImageInput(io::VFS& vfs, const std::string& path) {
    // Keep VFS access on its owning thread. Size is checked before allocating
    // encoded bytes; the eventual worker still owns an immutable byte vector.
    auto stream = vfs.Open(path);
    if (!stream || stream->Size() > kMaximumEncodedImageBytes) {
        SDL_SetError("Image is missing or exceeds the 128 MiB encoded input limit");
        return std::nullopt;
    }
    io::Bytes bytes(static_cast<std::size_t>(stream->Size()));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = stream->Read(bytes.data() + offset, bytes.size() - offset);
        if (count == 0) { SDL_SetError("Image input could not be read completely"); return std::nullopt; }
        offset += count;
    }
    if (stream->Failed()) { SDL_SetError("Image input stream failed"); return std::nullopt; }
    return bytes;
}

bool ImageDimensionsFit(const std::uint64_t width, const std::uint64_t height,
                        const std::size_t decodedLimit) {
    return width > 0 && height > 0 && width <= decodedLimit / 4u / height;
}

SDL_Surface* DecodeImage(const void* data, const std::size_t size,
                         const std::size_t decodedLimit) {
    if (!data || size == 0 || size > kMaximumEncodedImageBytes) {
        SDL_SetError("Image exceeds encoded input limit or has no data");
        return nullptr;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    constexpr std::uint8_t pngSignature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (size >= 24 && std::memcmp(bytes, pngSignature, sizeof(pngSignature)) == 0 &&
        std::memcmp(bytes + 12, "IHDR", 4) == 0) {
        const auto bigEndian = [bytes](const std::size_t offset) {
            return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
                   (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
                   (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
                   static_cast<std::uint32_t>(bytes[offset + 3]);
        };
        if (!ImageDimensionsFit(bigEndian(16), bigEndian(20), decodedLimit)) {
            SDL_SetError("PNG dimensions exceed decoded image byte limit");
            return nullptr;
        }
    }
    SDL_IOStream* stream = SDL_IOFromConstMem(data, size);
    SDL_Surface* surface = stream ? IMG_Load_IO(stream, true) : nullptr;
    if (surface && !ImageDimensionsFit(static_cast<std::uint64_t>(surface->w),
                                      static_cast<std::uint64_t>(surface->h), decodedLimit)) {
        SDL_DestroySurface(surface);
        SDL_SetError("Image dimensions exceed decoded image byte limit");
        return nullptr;
    }
    if (surface && (surface->pitch < 0 ||
                    static_cast<std::size_t>(surface->pitch) >
                        decodedLimit / static_cast<std::size_t>(surface->h))) {
        SDL_DestroySurface(surface);
        SDL_SetError("Decoded surface storage exceeds image byte limit");
        return nullptr;
    }
    // PNG is bounded before decompression. Other SDL_image codecs are checked
    // after decoding; this is not a universal pre-allocation decoder sandbox.
    return surface;
}

// Soft cap on resident textures; a long VN session visits far more backgrounds
// and sprites than are ever on screen together.
void AssetDiagnostic(std::string code,const std::string& path,std::string message,std::string details={}){
    diag::Diagnostic d{.severity=diag::Severity::Error,.code=std::move(code),.category="Asset.Load",.message=std::move(message),.details=std::move(details),.source={},.operationId={},.quickFix={}};d.source.path=path;diag::Emit(std::move(d));
}
}

AssetCache::AssetCache(SDL_Renderer* renderer, io::VFS& vfs) : m_renderer(renderer), m_vfs(vfs) {}

AssetCache::~AssetCache() {
    Clear();
}

void AssetCache::BeginFrame() {
    ++m_frame;
    HarvestPendingTextures();
    LaunchQueuedTextures();
    if (m_residentTextureBytes <= m_textureBudgetBytes) return;
    std::vector<std::pair<std::uint64_t, std::string>> order;
    order.reserve(m_textures.size());
    for (const auto& [path, entry] : m_textures) {
        // mem:// textures cannot be reloaded from disk; leave them alone.
        if (path.rfind("mem://", 0) == 0) {
            continue;
        }
        order.emplace_back(entry.lastUse, path);
    }
    std::sort(order.begin(), order.end());
    for (const auto& [lastUse, path] : order) {
        if (m_residentTextureBytes <= m_textureBudgetBytes) {
            break;
        }
        if (lastUse + 1 >= m_frame) {
            break;  // everything left was used last frame
        }
        auto it = m_textures.find(path);
        if (it != m_textures.end()) {
            m_residentTextureBytes-=std::min(m_residentTextureBytes,it->second.bytes);
            m_textures.erase(it);
        }
    }
}

void AssetCache::SetPreloadConcurrency(const std::size_t value) {
    m_preloadConcurrency = std::clamp<std::size_t>(value, 1, 32);
}

void AssetCache::SetPreloadFrameLimits(const std::size_t starts,
                                       const std::size_t uploads) {
    m_preloadStartsPerFrame = std::clamp<std::size_t>(starts, 1, 32);
    m_preloadUploadsPerFrame = std::clamp<std::size_t>(uploads, 1, 32);
}

void AssetCache::SetImageDecodeByteLimit(const std::size_t bytes) {
    m_imageDecodeByteLimit = std::max<std::size_t>(bytes, 4);
}

bool AssetCache::HasResidentTexture(const std::string& path) const {
    const auto found = m_textures.find(path);
    return found != m_textures.end() &&
           static_cast<bool>(found->second.texture);
}

AssetPreloadBatchId AssetCache::BeginTexturePreload(
    std::vector<AssetPreloadRequest> requests) {
    const AssetPreloadBatchId batchId = m_nextPreloadBatch++;
    BatchState batch;
    batch.progress.batchId = batchId;
    batch.progress.valid = true;
    m_preloadBatches.emplace(batchId, std::move(batch));

    std::unordered_set<std::string> unique;
    for (auto& request : requests) {
        if (!unique.insert(request.path).second) continue;
        auto& progress = m_preloadBatches.at(batchId).progress;
        ++progress.total;
        if (request.path.empty()) {
            CompleteBatchRequest(batchId, request.path, false, 0,
                                 "PXASSET7011",
                                 "Texture preload path is empty");
            continue;
        }
        QueueTexture(request.path, request.priority, batchId, false);
    }
    EmitBatchFailureIfComplete(m_preloadBatches.at(batchId));
    PruneBatchHistory();
    return batchId;
}

bool AssetCache::CancelTexturePreload(const AssetPreloadBatchId batchId) {
    const auto found = m_preloadBatches.find(batchId);
    if (found == m_preloadBatches.end() || found->second.progress.Finished())
        return false;
    auto& progress = found->second.progress;
    const std::size_t remaining = progress.total - progress.Completed();
    progress.cancelled += remaining;
    progress.queued = 0;
    progress.running = 0;

    for (auto iterator = m_queuedTextures.begin();
         iterator != m_queuedTextures.end();) {
        auto& batches = iterator->second.batches;
        std::erase(batches, batchId);
        if (batches.empty() && !iterator->second.cacheRequest)
            iterator = m_queuedTextures.erase(iterator);
        else
            ++iterator;
    }
    for (auto& pending : m_pendingTextures)
        std::erase(pending.batches, batchId);
    return true;
}

AssetPreloadProgress AssetCache::TexturePreloadProgress(
    const AssetPreloadBatchId batchId) const {
    const auto found = m_preloadBatches.find(batchId);
    return found == m_preloadBatches.end() ? AssetPreloadProgress{}
                                           : found->second.progress;
}

void AssetCache::BeginAssetSession() {
    ++m_assetSession;
    for (auto& batch : m_preloadBatches)
        (void)CancelTexturePreload(batch.first);
    m_queuedTextures.clear();
    // Running jobs own their input bytes and never touch the VFS or this
    // object. They stay in the harvest list so future destruction cannot block
    // the package/session switch; their session id prevents publication.
    DestroyResidentAssets();
}

void AssetCache::QueueTexture(const std::string& path,
                              const AssetPreloadPriority priority,
                              const AssetPreloadBatchId batchId,
                              const bool cacheRequest) {
    if (const auto resident = m_textures.find(path);
        resident != m_textures.end() && resident->second.texture) {
        if (batchId)
            CompleteBatchRequest(batchId, path, true, resident->second.bytes);
        return;
    }

    if (!m_asyncPreloadEnabled) {
        const TextureHandle texture = Texture(path);
        if (batchId) {
            CompleteBatchRequest(
                batchId, path, static_cast<bool>(texture),
                texture ? m_textures.at(path).bytes : 0,
                texture ? std::string{} : "PXASSET7012",
                texture ? std::string{}
                        : "Texture preload failed during synchronous decode");
        }
        return;
    }

    for (auto& pending : m_pendingTextures) {
        if (pending.session != m_assetSession || pending.path != path) continue;
        pending.cacheRequest = pending.cacheRequest || cacheRequest;
        if (batchId &&
            std::find(pending.batches.begin(), pending.batches.end(), batchId) ==
                pending.batches.end()) {
            pending.batches.push_back(batchId);
            ++m_preloadBatches.at(batchId).progress.running;
        }
        return;
    }

    auto [queued, inserted] = m_queuedTextures.try_emplace(
        path, QueuedTexture{.path = path,
                            .priority = priority,
                            .sequence = m_nextPreloadSequence++,
                            .session = m_assetSession,
                            .cacheRequest = cacheRequest,
                            .batches = {}});
    queued->second.priority = std::max(queued->second.priority, priority);
    queued->second.cacheRequest = queued->second.cacheRequest || cacheRequest;
    if (batchId &&
        std::find(queued->second.batches.begin(), queued->second.batches.end(),
                  batchId) == queued->second.batches.end()) {
        queued->second.batches.push_back(batchId);
        ++m_preloadBatches.at(batchId).progress.queued;
    }
    (void)inserted;
}

void AssetCache::LaunchQueuedTextures() {
    std::size_t started = 0;
    while (started < m_preloadStartsPerFrame &&
           m_pendingTextures.size() < m_preloadConcurrency &&
           !m_queuedTextures.empty()) {
        auto selected = std::max_element(
            m_queuedTextures.begin(), m_queuedTextures.end(),
            [](const auto& left, const auto& right) {
                if (left.second.priority != right.second.priority)
                    return left.second.priority < right.second.priority;
                return left.second.sequence > right.second.sequence;
            });
        QueuedTexture task = std::move(selected->second);
        m_queuedTextures.erase(selected);
        if (task.session != m_assetSession) continue;

        for (const auto batchId : task.batches) {
            const auto batch = m_preloadBatches.find(batchId);
            if (batch == m_preloadBatches.end()) continue;
            if (batch->second.progress.queued > 0)
                --batch->second.progress.queued;
            ++batch->second.progress.running;
        }

        ++started;
        auto bytes = ReadImageInput(m_vfs, task.path);
        if (!bytes) {
            for (const auto batchId : task.batches)
                CompleteBatchRequest(batchId, task.path, false, 0,
                                     "PXASSET7013",
                                     "Texture preload asset is missing, unreadable, or exceeds the encoded input limit");
            if (task.cacheRequest)
                AssetDiagnostic("PXASSET7013", task.path,
                                "Texture preload asset is missing, unreadable, or exceeds the encoded input limit");
            continue;
        }

        try {
            auto future = std::async(
                std::launch::async,
                [input = std::move(*bytes), limit = m_imageDecodeByteLimit]() mutable -> DecodeResult {
                    SDL_Surface* surface = DecodeImage(input.data(), input.size(), limit);
                    if (!surface)
                        return {.surface = nullptr,
                                .width = 0,
                                .height = 0,
                                .decodedBytes = 0,
                                .error = SDL_GetError()};
                    const std::size_t decodedBytes =
                        static_cast<std::size_t>(surface->w) *
                        static_cast<std::size_t>(surface->h) * 4u;
                    return {.surface = surface,
                            .width = surface->w,
                            .height = surface->h,
                            .decodedBytes = decodedBytes,
                            .error = {}};
                });
            m_pendingTextures.push_back(
                {.path = std::move(task.path),
                 .session = task.session,
                 .cacheRequest = task.cacheRequest,
                 .batches = std::move(task.batches),
                 .future = std::move(future)});
        } catch (const std::exception& error) {
            for (const auto batchId : task.batches)
                CompleteBatchRequest(batchId, task.path, false, 0,
                                     "PXASSET7014", error.what());
            if (task.cacheRequest)
                AssetDiagnostic("PXASSET7014", task.path,
                                "Texture preload worker could not start",
                                error.what());
        }
    }
}

void AssetCache::HarvestPendingTextures() {
    std::size_t uploaded = 0;
    for (auto iterator = m_pendingTextures.begin();
         iterator != m_pendingTextures.end();) {
        if (!iterator->future.valid()) {
            iterator = m_pendingTextures.erase(iterator);
            continue;
        }
        if (iterator->future.wait_for(std::chrono::seconds(0)) !=
            std::future_status::ready) {
            ++iterator;
            continue;
        }

        const bool currentSession = iterator->session == m_assetSession;
        const bool requested = iterator->cacheRequest || !iterator->batches.empty();
        if (currentSession && requested && uploaded >= m_preloadUploadsPerFrame) {
            ++iterator;
            continue;
        }
        if (currentSession && requested) ++uploaded;
        DecodeResult decoded;
        try {
            decoded = iterator->future.get();
        } catch (const std::exception& error) {
            decoded.error = error.what();
        } catch (...) {
            decoded.error = "unknown texture decode failure";
        }

        SDL_Texture* nativeTexture = nullptr;
        if (currentSession && requested && decoded.surface && m_renderer)
            nativeTexture = SDL_CreateTextureFromSurface(m_renderer, decoded.surface);
        if (decoded.surface) SDL_DestroySurface(decoded.surface);
        TextureResource texture = TextureResource::Adopt(
            TextureBackend::SdlRenderer, nativeTexture, decoded.width,
            decoded.height, iterator->session);
        if (texture) (void)texture.SetAlphaBlend();

        const bool succeeded = currentSession && requested &&
                               static_cast<bool>(texture);
        if (succeeded) {
            if (const auto existing = m_textures.find(iterator->path);
                existing != m_textures.end()) {
                m_residentTextureBytes -=
                    std::min(m_residentTextureBytes, existing->second.bytes);
            }
            m_textures[iterator->path] =
                {std::move(texture), m_frame, decoded.decodedBytes};
            m_residentTextureBytes += decoded.decodedBytes;
        }

        if (currentSession) {
            const std::string message = decoded.error.empty()
                ? "Texture preload could not create a renderer texture"
                : "Texture preload decode failed: " + decoded.error;
            for (const auto batchId : iterator->batches)
                CompleteBatchRequest(batchId, iterator->path, succeeded,
                                     succeeded ? decoded.decodedBytes : 0,
                                     succeeded ? std::string{}
                                               : "PXASSET7015",
                                     succeeded ? std::string{} : message);
            if (iterator->cacheRequest && !succeeded)
                AssetDiagnostic("PXASSET7015", iterator->path,
                                "Texture preload failed", decoded.error);
        }
        iterator = m_pendingTextures.erase(iterator);
    }
}

void AssetCache::CompleteBatchRequest(
    const AssetPreloadBatchId batchId, const std::string& path,
    const bool succeeded, const std::size_t decodedBytes, std::string code,
    std::string message) {
    const auto found = m_preloadBatches.find(batchId);
    if (found == m_preloadBatches.end() || found->second.progress.Finished())
        return;
    auto& progress = found->second.progress;
    if (progress.running > 0)
        --progress.running;
    else if (progress.queued > 0)
        --progress.queued;
    if (succeeded) {
        ++progress.succeeded;
        progress.decodedBytes += decodedBytes;
    } else {
        ++progress.failed;
        progress.failures.push_back(
            {.path = path,
             .code = std::move(code),
             .message = std::move(message)});
    }
    EmitBatchFailureIfComplete(found->second);
}

void AssetCache::EmitBatchFailureIfComplete(BatchState& batch) {
    if (!batch.progress.Finished() || batch.progress.failures.empty() ||
        batch.failureDiagnosticEmitted)
        return;
    batch.failureDiagnosticEmitted = true;
    std::ostringstream details;
    details << batch.progress.failures.size() << " asset(s) failed";
    const std::size_t shown =
        std::min<std::size_t>(batch.progress.failures.size(), 8);
    for (std::size_t index = 0; index < shown; ++index)
        details << "\n" << batch.progress.failures[index].path << ": "
                << batch.progress.failures[index].message;
    AssetDiagnostic("PXASSET7010", "",
                    "Texture preload batch completed with failures",
                    details.str());
}

void AssetCache::PruneBatchHistory() {
    constexpr std::size_t kMaximumBatchHistory = 128;
    if (m_preloadBatches.size() <= kMaximumBatchHistory) return;
    std::vector<AssetPreloadBatchId> completed;
    for (const auto& [id, batch] : m_preloadBatches)
        if (batch.progress.Finished()) completed.push_back(id);
    std::sort(completed.begin(), completed.end());
    for (const auto id : completed) {
        if (m_preloadBatches.size() <= kMaximumBatchHistory) break;
        m_preloadBatches.erase(id);
    }
}

TextureHandle AssetCache::Texture(const std::string& path) {
    if (auto it = m_textures.find(path); it != m_textures.end()) {
        it->second.lastUse = m_frame;
        return it->second.texture.Handle();
    }
    if (m_queuedTextures.contains(path)) return {};
    if (std::any_of(m_pendingTextures.begin(), m_pendingTextures.end(),
                    [this, &path](const PendingTexture& pending) {
                        return pending.session == m_assetSession &&
                               pending.path == path;
                    }))
        return {};

    auto bytes = ReadImageInput(m_vfs, path);
    if (!bytes) {
        PX_LOG_WARN("AssetCache: texture not found '{}'", path);
        AssetDiagnostic("PXASSET7001",path,"Texture asset is missing, unreadable, or exceeds the encoded input limit");
        m_textures[path] = TextureEntry{{}, m_frame,0};
        return {};
    }

    SDL_Surface* surface = DecodeImage(bytes->data(), bytes->size(), m_imageDecodeByteLimit);
    if (!surface) {
        PX_LOG_WARN("AssetCache: failed to decode image '{}': {}", path, SDL_GetError());
        AssetDiagnostic("PXASSET7002",path,"Texture asset could not be decoded",SDL_GetError());
        m_textures[path] = TextureEntry{{}, m_frame,0};
        return {};
    }

    SDL_Texture* nativeTexture =
        SDL_CreateTextureFromSurface(m_renderer, surface);
    const int width = surface->w;
    const int height = surface->h;
    SDL_DestroySurface(surface);
    TextureResource texture = TextureResource::Adopt(
        TextureBackend::SdlRenderer, nativeTexture, width, height,
        m_assetSession);
    if (texture) (void)texture.SetAlphaBlend();
    const std::size_t textureBytes = texture
        ? static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
              4u
        : 0;
    m_residentTextureBytes += textureBytes;
    m_textures[path] =
        TextureEntry{std::move(texture), m_frame, textureBytes};
    return m_textures[path].texture.Handle();
}

void AssetCache::PreloadTexture(const std::string& path,
                                const AssetPreloadPriority priority) {
    if (path.empty()) return;
    QueueTexture(path, priority, 0, true);
}

SDL_Surface* AssetCache::LoadSurface(const std::string& path) {
    auto bytes = ReadImageInput(m_vfs, path);
    if (!bytes) {
        PX_LOG_WARN("AssetCache: surface not found '{}'", path);
        AssetDiagnostic("PXASSET7003",path,"Image surface is missing, unreadable, or exceeds the encoded input limit");
        return nullptr;
    }
    SDL_Surface* surface = DecodeImage(bytes->data(), bytes->size(), m_imageDecodeByteLimit);
    if (!surface) {
        PX_LOG_WARN("AssetCache: failed to decode surface '{}': {}", path, SDL_GetError());
        AssetDiagnostic("PXASSET7004",path,"Image surface could not be decoded",SDL_GetError());
        return nullptr;
    }
    SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surface);
    return rgba;
}

TextureHandle AssetCache::RegisterMemoryTexture(const std::string& key,
                                                const void* data,
                                                const std::size_t size) {
    UnregisterTexture(key);
    if (!data || size == 0) {
        AssetDiagnostic("PXASSET7005",key,"Memory texture has no data");
        return {};
    }
    SDL_Surface* surface = DecodeImage(data, size, m_imageDecodeByteLimit);
    if (!surface) {
        PX_LOG_WARN("AssetCache: failed to decode memory image '{}': {}", key, SDL_GetError());
        AssetDiagnostic("PXASSET7006",key,"Memory texture could not be decoded",SDL_GetError());
        return {};
    }
    SDL_Texture* nativeTexture =
        SDL_CreateTextureFromSurface(m_renderer, surface);
    const int width = surface->w;
    const int height = surface->h;
    SDL_DestroySurface(surface);
    TextureResource texture = TextureResource::Adopt(
        TextureBackend::SdlRenderer, nativeTexture, width, height,
        m_assetSession);
    if (texture) (void)texture.SetAlphaBlend();
    const std::size_t textureBytes = texture
        ? static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
              4u
        : 0;
    m_residentTextureBytes += textureBytes;
    m_textures[key] =
        TextureEntry{std::move(texture), m_frame, textureBytes};
    return m_textures[key].texture.Handle();
}

void AssetCache::UnregisterTexture(const std::string& key) {
    if (auto it = m_textures.find(key); it != m_textures.end()) {
        m_residentTextureBytes-=std::min(m_residentTextureBytes,it->second.bytes);
        m_textures.erase(it);
    }
}

TTF_Font* AssetCache::Font(const std::string& path, int size, int outline) {
    const std::string key = path + "|" + std::to_string(size) + "|" + std::to_string(outline);
    if (auto it = m_fonts.find(key); it != m_fonts.end()) {
        return it->second.font;
    }

    auto bytes = m_vfs.Read(path);
    std::string resolvedPath = path;
    if (!bytes && path == "Content/Fonts/NotoSansTC-Bold.ttf") {
        resolvedPath = "Resources/Fonts/NotoSansTC-Bold.ttf";
        bytes = m_vfs.Read(resolvedPath);
        if (bytes) {
            PX_LOG_WARN("AssetCache: project font missing '{}'; using fallback '{}'", path,
                        resolvedPath);
        }
    }
    if (!bytes) {
        PX_LOG_WARN("AssetCache: font not found '{}'", path);
        AssetDiagnostic("PXASSET7007",path,"Font asset was not found");
        m_fonts[key] = {};
        return nullptr;
    }

    auto held = std::make_shared<io::Bytes>(std::move(*bytes));
    SDL_IOStream* io = SDL_IOFromConstMem(held->data(), held->size());
    TTF_Font* font = io ? TTF_OpenFontIO(io, /*closeio=*/true, static_cast<float>(size)) : nullptr;
    if (!font) {
        PX_LOG_WARN("AssetCache: failed to open font '{}': {}", resolvedPath, SDL_GetError());
        AssetDiagnostic("PXASSET7008",resolvedPath,"Font asset could not be opened",SDL_GetError());
        m_fonts[key] = {};
        return nullptr;
    }
    if (outline > 0) {
        TTF_SetFontOutline(font, outline);
    }
    m_fonts[key] = FontEntry{ font, held };
    if (!m_configuringFontFallbacks && !m_fontFallbackChain.empty()) {
        m_configuringFontFallbacks = true;
        for (const auto& fallbackPath : m_fontFallbackChain) {
            if (fallbackPath == path || fallbackPath == resolvedPath) continue;
            if (TTF_Font* fallback = Font(fallbackPath, size, outline))
                (void)TTF_AddFallbackFont(font, fallback);
        }
        m_configuringFontFallbacks = false;
    }
    return font;
}

bool AssetCache::SetFontFallbackChain(std::vector<std::string> paths) {
    paths.erase(std::remove_if(paths.begin(), paths.end(),
                               [](const auto& path) { return path.empty(); }),
                paths.end());
    if (paths == m_fontFallbackChain) return false;
    DestroyFonts();
    m_fontFallbackChain = std::move(paths);
    return true;
}

void AssetCache::TextureSize(const TextureHandle texture, int& w, int& h) {
    w = texture.Width();
    h = texture.Height();
}

void AssetCache::DestroyResidentAssets() {
    for (auto& [path, entry] : m_textures) {
        (void)path;
        entry.texture.Reset();
    }
    m_textures.clear();
    m_residentTextureBytes=0;
    DestroyFonts();
}

void AssetCache::DestroyFonts() {
    for (auto& [key, entry] : m_fonts) {
        (void)key;
        if (entry.font) TTF_ClearFallbackFonts(entry.font);
    }
    for (auto& [key, entry] : m_fonts) {
        (void)key;
        if (entry.font) {
            TTF_CloseFont(entry.font);
        }
    }
    m_fonts.clear();
}

void AssetCache::Clear() {
    BeginAssetSession();
    for (auto& pending : m_pendingTextures) {
        if (!pending.future.valid()) continue;
        try {
            DecodeResult decoded = pending.future.get();
            if (decoded.surface) SDL_DestroySurface(decoded.surface);
        } catch (...) {
        }
    }
    m_pendingTextures.clear();
    m_preloadBatches.clear();
}

}
