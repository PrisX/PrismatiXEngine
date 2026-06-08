#include "Engine/Gfx/AssetCache.h"

#include "Logger.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

namespace px::gfx {

AssetCache::AssetCache(SDL_Renderer* renderer, io::Vfs& vfs) : m_renderer(renderer), m_vfs(vfs) {}

AssetCache::~AssetCache() {
    Clear();
}

SDL_Texture* AssetCache::Texture(const std::string& path) {
    if (auto it = m_textures.find(path); it != m_textures.end()) {
        return it->second;
    }

    auto bytes = m_vfs.Read(path);
    if (!bytes) {
        PX_LOG_WARN("AssetCache: texture not found '{}'", path);
        m_textures[path] = nullptr;
        return nullptr;
    }

    SDL_IOStream* io = SDL_IOFromConstMem(bytes->data(), bytes->size());
    SDL_Surface* surface = io ? IMG_Load_IO(io, /*closeio=*/true) : nullptr;
    if (!surface) {
        PX_LOG_WARN("AssetCache: failed to decode image '{}': {}", path, SDL_GetError());
        m_textures[path] = nullptr;
        return nullptr;
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(m_renderer, surface);
    SDL_DestroySurface(surface);
    if (texture) {
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }
    m_textures[path] = texture;
    return texture;
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
    for (auto& [path, texture] : m_textures) {
        if (texture) {
            SDL_DestroyTexture(texture);
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
