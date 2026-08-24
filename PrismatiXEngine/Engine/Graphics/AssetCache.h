#pragma once

#include "Engine/IO/VFS.h"

#include <cstdint>
#include <memory>
#include <future>
#include <string>
#include <unordered_map>

struct SDL_Renderer;
struct SDL_Surface;
struct SDL_Texture;
struct TTF_Font;

namespace px::graphics {

class AssetCache {
public:
    AssetCache(SDL_Renderer* renderer, io::VFS& vfs);
    ~AssetCache();

    AssetCache(const AssetCache&) = delete;
    AssetCache& operator=(const AssetCache&) = delete;

    [[nodiscard]] SDL_Texture* Texture(const std::string& path);
    void PreloadTexture(const std::string& path);
    void SetAsyncPreloadEnabled(bool enabled) { m_asyncPreloadEnabled = enabled; }
    [[nodiscard]] bool AsyncPreloadEnabled() const { return m_asyncPreloadEnabled; }
    void SetTextureBudget(std::size_t bytes) { m_textureBudgetBytes=bytes; }
    [[nodiscard]] std::size_t ResidentTextureBytes() const { return m_residentTextureBytes; }

    // Call once per frame before any drawing: advances the LRU clock and evicts
    // textures that were not used recently (deferred so nothing in-flight on the
    // render queue is destroyed mid-frame).
    void BeginFrame();

    // Decodes an image into an uncached RGBA32 surface (rule transitions need
    // CPU pixel access). Caller destroys the surface.
    [[nodiscard]] SDL_Surface* LoadSurface(const std::string& path);

    // Decodes an in-memory image (e.g. a save thumbnail) and caches it under a
    // virtual key (convention: "mem://..."). Replaces any previous registration.
    SDL_Texture* RegisterMemoryTexture(const std::string& key, const void* data,
                                       std::size_t size);
    void UnregisterTexture(const std::string& key);

    [[nodiscard]] TTF_Font* Font(const std::string& path, int size, int outline = 0);

    static void TextureSize(SDL_Texture* texture, int& w, int& h);

    void Clear();

private:
    struct FontEntry {
        TTF_Font* font = nullptr;
        std::shared_ptr<io::Bytes> bytes;
    };

    struct TextureEntry {
        SDL_Texture* texture = nullptr;
        std::uint64_t lastUse = 0;
        std::size_t bytes = 0;
    };

    SDL_Renderer* m_renderer;
    io::VFS& m_vfs;
    std::unordered_map<std::string, TextureEntry> m_textures;
    std::unordered_map<std::string,std::future<SDL_Surface*>> m_pendingTextures;
    std::unordered_map<std::string, FontEntry> m_fonts;
    std::uint64_t m_frame = 0;
    std::size_t m_textureBudgetBytes=512u*1024u*1024u;
    std::size_t m_residentTextureBytes=0;
    bool m_asyncPreloadEnabled=true;
};

}
