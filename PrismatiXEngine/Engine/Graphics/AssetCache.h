#pragma once

#include "Engine/IO/VFS.h"

#include <memory>
#include <string>
#include <unordered_map>

struct SDL_Renderer;
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

    [[nodiscard]] TTF_Font* Font(const std::string& path, int size, int outline = 0);

    static void TextureSize(SDL_Texture* texture, int& w, int& h);

    void Clear();

private:
    struct FontEntry {
        TTF_Font* font = nullptr;
        std::shared_ptr<io::Bytes> bytes;
    };

    SDL_Renderer* m_renderer;
    io::VFS& m_vfs;
    std::unordered_map<std::string, SDL_Texture*> m_textures;
    std::unordered_map<std::string, FontEntry> m_fonts;
};

}
