#include "Engine/Graphics/AssetCache.h"

#include "Engine/Support/Logger.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace px::graphics {

namespace {
// Soft cap on resident textures; a long VN session visits far more backgrounds
// and sprites than are ever on screen together.
constexpr std::size_t kMaxTextures = 128;
}

AssetCache::AssetCache(SDL_Renderer* renderer, io::VFS& vfs) : m_renderer(renderer), m_vfs(vfs) {}

AssetCache::~AssetCache() {
    Clear();
}

void AssetCache::BeginFrame() {
    ++m_frame;
    if (m_textures.size() <= kMaxTextures) {
        return;
    }
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
        if (m_textures.size() <= kMaxTextures) {
            break;
        }
        if (lastUse + 1 >= m_frame) {
            break;  // everything left was used last frame
        }
        auto it = m_textures.find(path);
        if (it != m_textures.end()) {
            if (it->second.texture) {
                SDL_DestroyTexture(it->second.texture);
            }
            m_textures.erase(it);
        }
    }
}

SDL_Texture* AssetCache::Texture(const std::string& path) {
    if (auto it = m_textures.find(path); it != m_textures.end()) {
        it->second.lastUse = m_frame;
        return it->second.texture;
    }

    auto bytes = m_vfs.Read(path);
    if (!bytes) {
        PX_LOG_WARN("AssetCache: texture not found '{}'", path);
        m_textures[path] = TextureEntry{ nullptr, m_frame };
        return nullptr;
    }

    SDL_IOStream* io = SDL_IOFromConstMem(bytes->data(), bytes->size());
    SDL_Surface* surface = io ? IMG_Load_IO(io, /*closeio=*/true) : nullptr;
    if (!surface) {
        PX_LOG_WARN("AssetCache: failed to decode image '{}': {}", path, SDL_GetError());
        m_textures[path] = TextureEntry{ nullptr, m_frame };
        return nullptr;
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(m_renderer, surface);
    SDL_DestroySurface(surface);
    if (texture) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }
    m_textures[path] = TextureEntry{ texture, m_frame };
    return texture;
}

SDL_Surface* AssetCache::LoadSurface(const std::string& path) {
    auto bytes = m_vfs.Read(path);
    if (!bytes) {
        PX_LOG_WARN("AssetCache: surface not found '{}'", path);
        return nullptr;
    }
    SDL_IOStream* io = SDL_IOFromConstMem(bytes->data(), bytes->size());
    SDL_Surface* surface = io ? IMG_Load_IO(io, /*closeio=*/true) : nullptr;
    if (!surface) {
        PX_LOG_WARN("AssetCache: failed to decode surface '{}': {}", path, SDL_GetError());
        return nullptr;
    }
    SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(surface);
    return rgba;
}

SDL_Texture* AssetCache::RegisterMemoryTexture(const std::string& key, const void* data,
                                               std::size_t size) {
    UnregisterTexture(key);
    if (!data || size == 0) {
        return nullptr;
    }
    SDL_IOStream* io = SDL_IOFromConstMem(data, size);
    SDL_Surface* surface = io ? IMG_Load_IO(io, /*closeio=*/true) : nullptr;
    if (!surface) {
        PX_LOG_WARN("AssetCache: failed to decode memory image '{}': {}", key, SDL_GetError());
        return nullptr;
    }
    SDL_Texture* texture = SDL_CreateTextureFromSurface(m_renderer, surface);
    SDL_DestroySurface(surface);
    if (texture) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }
    m_textures[key] = TextureEntry{ texture, m_frame };
    return texture;
}

void AssetCache::UnregisterTexture(const std::string& key) {
    if (auto it = m_textures.find(key); it != m_textures.end()) {
        if (it->second.texture) {
            SDL_DestroyTexture(it->second.texture);
        }
        m_textures.erase(it);
    }
}

TTF_Font* AssetCache::Font(const std::string& path, int size, int outline) {
    const std::string key = path + "|" + std::to_string(size) + "|" + std::to_string(outline);
    if (auto it = m_fonts.find(key); it != m_fonts.end()) {
        return it->second.font;
    }

    auto bytes = m_vfs.Read(path);
    if (!bytes) {
        PX_LOG_WARN("AssetCache: font not found '{}'", path);
        m_fonts[key] = {};
        return nullptr;
    }

    auto held = std::make_shared<io::Bytes>(std::move(*bytes));
    SDL_IOStream* io = SDL_IOFromConstMem(held->data(), held->size());
    TTF_Font* font = io ? TTF_OpenFontIO(io, /*closeio=*/true, static_cast<float>(size)) : nullptr;
    if (!font) {
        PX_LOG_WARN("AssetCache: failed to open font '{}': {}", path, SDL_GetError());
        m_fonts[key] = {};
        return nullptr;
    }
    if (outline > 0) {
        TTF_SetFontOutline(font, outline);
    }
    m_fonts[key] = FontEntry{ font, held };
    return font;
}

void AssetCache::TextureSize(SDL_Texture* texture, int& w, int& h) {
    w = 0;
    h = 0;
    if (texture) {
        float fw = 0.0f, fh = 0.0f;
        SDL_GetTextureSize(texture, &fw, &fh);
        w = static_cast<int>(fw);
        h = static_cast<int>(fh);
    }
}

void AssetCache::Clear() {
    for (auto& [path, entry] : m_textures) {
        if (entry.texture) {
            SDL_DestroyTexture(entry.texture);
        }
    }
    m_textures.clear();
    for (auto& [key, entry] : m_fonts) {
        if (entry.font) {
            TTF_CloseFont(entry.font);
        }
    }
    m_fonts.clear();
}

}
