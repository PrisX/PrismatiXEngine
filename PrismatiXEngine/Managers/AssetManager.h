#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL_mixer.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../Utils/LRUCache.h"

class ArchiveManager;

class AssetManager {
private:
    ArchiveManager& archiveManager;

    std::unique_ptr<LRUCache<std::string, SDL_Texture*>> textureCache;
    std::unique_ptr<LRUCache<std::string, TTF_Font*>> fontCache;
    std::unordered_map<std::string, TTF_Font*> outlineFontCache;  // Outline fonts are usually few, can be tied to base font or also LRU
    std::unordered_map<std::string, int> fontSizeByKey;
    std::unordered_map<TTF_Font*, std::string> fontReverseMap;
    std::unique_ptr<LRUCache<std::string, Mix_Chunk*>> sfxCache;

    std::unordered_map<std::string, std::vector<char>> fontBuffers;

    // Cache key for fonts (name + size)
    std::string GetFontKey(const std::string& fileName, int fontSize);

public:
    AssetManager(ArchiveManager& archiveMgr, size_t texLimit = 50, size_t fontLimit = 10, size_t sfxLimit = 20);
    ~AssetManager();

    SDL_Texture* LoadTexture(const std::string& fileName, SDL_Renderer* ren);
    TTF_Font* LoadFont(const std::string& fileName, int fontSize);
    TTF_Font* GetOutlineFont(TTF_Font* baseFont, int outlineSize);
    Mix_Chunk* LoadSFX(const std::string& fileName);
    Mix_Music* LoadBGM(const std::string& fileName, std::vector<char>& outBuffer);

    void CleanTextures();
    void CleanFonts();
    void CleanAudio();
    void CleanAll();
};
