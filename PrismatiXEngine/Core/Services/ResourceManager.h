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

struct PDXFileEntry {
    std::string archivePath;
    uint64_t offset;
    uint64_t size;
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
    std::unique_ptr<LRUCache<std::string, SDL_Texture*>> textureCache;
    std::unique_ptr<LRUCache<std::string, TTF_Font*>> fontCache;
    std::unordered_map<std::string, TTF_Font*> outlineFontCache;
    std::unordered_map<std::string, int> fontSizeByKey;
    std::unordered_map<TTF_Font*, std::string> fontReverseMap;
    std::unique_ptr<LRUCache<std::string, Mix_Chunk*>> sfxCache;

    std::unordered_map<std::string, std::vector<char>> fontBuffers;

    std::string GetFontKey(const std::string& fileName, int fontSize);
};
