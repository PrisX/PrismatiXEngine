#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL_mixer.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Utils/LRUCache.h"

namespace PrismatiX::Services {

struct PDXFileEntry {
    std::string archivePath;
    uint64_t offset;
    uint64_t size;
};

struct FontAsset {
    TTF_Font* baseFont = nullptr;
    std::vector<TTF_Font*> outlineFonts;
    std::vector<char> buffer;
    int size = 0;

    ~FontAsset() {
        if (baseFont) TTF_CloseFont(baseFont);
        for (auto* font : outlineFonts) {
            if (font) TTF_CloseFont(font);
        }
    }
};

class ResourceManager {
public:
    ResourceManager(SDL_Renderer* renderer, size_t texLimit = 50, size_t fontLimit = 10, size_t sfxLimit = 20);
    ~ResourceManager();

    // Archive & File Mounting
    bool MountArchive(const std::string& archivePath);
    void ScanDirectory(const std::string& root);
    std::vector<char> ExtractFile(const std::string& fileName);
    std::string LoadText(const std::string& path);

    // Asset Loading & Caching
    SDL_Texture* LoadTexture(const std::string& fileName);
    TTF_Font* LoadFont(const std::string& fileName, int fontSize);
    TTF_Font* GetOutlineFont(TTF_Font* baseFont, int outlineSize);
    Mix_Chunk* LoadSFX(const std::string& fileName);
    Mix_Music* LoadBGM(const std::string& fileName, std::vector<char>& outBuffer);

    // Cleanup
    void CleanTextures();
    void CleanFonts();
    void CleanAudio();
    void CleanAll();

private:
    SDL_Renderer* renderer;

    // Archive data
    std::unordered_map<std::string, PDXFileEntry> globalFileTable;
    std::unordered_map<std::string, std::string> diskFileMap;  // filename -> full path

    // Cache data
    std::unique_ptr<PrismatiX::Utils::LRUCache<std::string, SDL_Texture*>> textureCache;
    std::unique_ptr<PrismatiX::Utils::LRUCache<std::string, FontAsset*>> fontCache;
    std::unique_ptr<PrismatiX::Utils::LRUCache<std::string, Mix_Chunk*>> sfxCache;

    // Helper map to find FontAsset by base font pointer
    std::unordered_map<TTF_Font*, std::string> fontReverseMap;

    std::string GetFontKey(const std::string& fileName, int fontSize);
};

} // namespace PrismatiX::Services
