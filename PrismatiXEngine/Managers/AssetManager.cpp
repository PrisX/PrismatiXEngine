#include "AssetManager.h"

#include <SDL2/SDL_image.h>

#include <iostream>

#include "ArchiveManager.h"

AssetManager::AssetManager(ArchiveManager& archiveMgr, size_t texLimit, size_t fontLimit, size_t sfxLimit) : archiveManager(archiveMgr) {
    textureCache = std::make_unique<LRUCache<std::string, SDL_Texture*>>(texLimit, [](SDL_Texture* tex) {
        if (tex) SDL_DestroyTexture(tex);
    });

    sfxCache = std::make_unique<LRUCache<std::string, Mix_Chunk*>>(sfxLimit, [](Mix_Chunk* chunk) {
        if (chunk) Mix_FreeChunk(chunk);
    });

    fontCache = std::make_unique<LRUCache<std::string, TTF_Font*>>(fontLimit, [this](TTF_Font* font) {
        if (font) {
            auto it = fontReverseMap.find(font);
            if (it != fontReverseMap.end()) {
                std::string key = it->second;

                for (auto olIt = outlineFontCache.begin(); olIt != outlineFontCache.end();) {
                    if (olIt->first.find(key + "_ol") == 0) {
                        TTF_CloseFont(olIt->second);
                        olIt = outlineFontCache.erase(olIt);
                    }
                    else {
                        ++olIt;
                    }
                }

                fontReverseMap.erase(it);
                fontSizeByKey.erase(key);
                fontBuffers.erase(key);
            }
            TTF_CloseFont(font);
        }
    });
}

AssetManager::~AssetManager() { CleanAll(); }

std::string AssetManager::GetFontKey(const std::string& fileName, int fontSize) { return fileName + "_" + std::to_string(fontSize); }

SDL_Texture* AssetManager::LoadTexture(const std::string& fileName, SDL_Renderer* ren) {
    SDL_Texture* tex = nullptr;
    if (textureCache->Get(fileName, tex)) {
        return tex;
    }

    std::vector<char> buffer = archiveManager.ExtractFile(fileName);
    if (buffer.empty()) return nullptr;

    SDL_RWops* rw = SDL_RWFromMem(buffer.data(), (int)buffer.size());
    if (!rw) return nullptr;

    tex = IMG_LoadTexture_RW(ren, rw, 1);
    if (!tex) {
        std::cerr << "AssetManager failed to load image (" << fileName << "): " << IMG_GetError() << std::endl;
        return nullptr;
    }

    textureCache->Put(fileName, tex);
    return tex;
}

TTF_Font* AssetManager::LoadFont(const std::string& fileName, int fontSize) {
    std::string key = GetFontKey(fileName, fontSize);
    TTF_Font* font = nullptr;
    if (fontCache->Get(key, font)) {
        return font;
    }

    std::vector<char> buffer = archiveManager.ExtractFile(fileName);
    if (buffer.empty()) return nullptr;

    fontBuffers[key] = std::move(buffer);
    SDL_RWops* rw = SDL_RWFromMem(fontBuffers[key].data(), (int)fontBuffers[key].size());
    if (!rw) return nullptr;

    font = TTF_OpenFontRW(rw, 1, fontSize * 2);  // temp oversample
    if (!font) {
        std::cerr << "AssetManager failed to load font (" << fileName << "): " << TTF_GetError() << std::endl;
        fontBuffers.erase(key);
        return nullptr;
    }

    fontCache->Put(key, font);
    fontReverseMap[font] = key;
    fontSizeByKey[key] = fontSize;
    return font;
}

TTF_Font* AssetManager::GetOutlineFont(TTF_Font* baseFont, int outlineSize) {
    auto keyIt = fontReverseMap.find(baseFont);
    if (keyIt == fontReverseMap.end()) return nullptr;

    const std::string& baseKey = keyIt->second;
    std::string olKey = baseKey + "_ol" + std::to_string(outlineSize);

    auto cached = outlineFontCache.find(olKey);
    if (cached != outlineFontCache.end()) return cached->second;

    auto bufIt = fontBuffers.find(baseKey);
    auto sizeIt = fontSizeByKey.find(baseKey);
    if (bufIt == fontBuffers.end() || sizeIt == fontSizeByKey.end()) return nullptr;

    SDL_RWops* rw = SDL_RWFromMem(bufIt->second.data(), (int)bufIt->second.size());
    TTF_Font* olFont = TTF_OpenFontRW(rw, 1, sizeIt->second * 2);
    if (!olFont) return nullptr;

    TTF_SetFontOutline(olFont, outlineSize * 2);
    outlineFontCache[olKey] = olFont;
    return olFont;
}

Mix_Chunk* AssetManager::LoadSFX(const std::string& fileName) {
    Mix_Chunk* chunk = nullptr;
    if (sfxCache->Get(fileName, chunk)) {
        return chunk;
    }

    std::vector<char> buffer = archiveManager.ExtractFile(fileName);
    if (buffer.empty()) return nullptr;

    SDL_RWops* rw = SDL_RWFromMem(buffer.data(), (int)buffer.size());
    if (!rw) return nullptr;

    chunk = Mix_LoadWAV_RW(rw, 1);
    if (!chunk) {
        std::cerr << "AssetManager failed to load SFX (" << fileName << "): " << Mix_GetError() << std::endl;
        return nullptr;
    }

    sfxCache->Put(fileName, chunk);
    return chunk;
}

Mix_Music* AssetManager::LoadBGM(const std::string& fileName, std::vector<char>& outBuffer) {
    outBuffer = archiveManager.ExtractFile(fileName);
    if (outBuffer.empty()) return nullptr;

    SDL_RWops* rw = SDL_RWFromMem(outBuffer.data(), (int)outBuffer.size());
    if (!rw) return nullptr;

    Mix_Music* music = Mix_LoadMUS_RW(rw, 1);
    if (!music) {
        std::cerr << "AssetManager failed to load BGM (" << fileName << "): " << Mix_GetError() << std::endl;
    }

    return music;
}

void AssetManager::CleanTextures() { textureCache->Clear(); }

void AssetManager::CleanFonts() {
    fontCache->Clear();
    for (auto& pair : outlineFontCache) {
        if (pair.second) TTF_CloseFont(pair.second);
    }
    outlineFontCache.clear();
    fontBuffers.clear();
    fontSizeByKey.clear();
    fontReverseMap.clear();
}

void AssetManager::CleanAudio() { sfxCache->Clear(); }

void AssetManager::CleanAll() {
    CleanTextures();
    CleanFonts();
    CleanAudio();
}
