#include "AssetManager.h"

#include <SDL2/SDL_image.h>

#include <iostream>

#include "ArchiveManager.h"

AssetManager::AssetManager(ArchiveManager& archiveMgr) : archiveManager(archiveMgr) {}

AssetManager::~AssetManager() { CleanAll(); }

std::string AssetManager::GetFontKey(const std::string& fileName, int fontSize) { return fileName + "_" + std::to_string(fontSize); }

SDL_Texture* AssetManager::LoadTexture(const std::string& fileName, SDL_Renderer* ren) {
    if (textureCache.find(fileName) != textureCache.end()) {
        return textureCache[fileName];
    }

    std::vector<char> buffer = archiveManager.ExtractFile(fileName);
    if (buffer.empty()) return nullptr;

    SDL_RWops* rw = SDL_RWFromMem(buffer.data(), buffer.size());
    if (!rw) return nullptr;

    SDL_Texture* tex = IMG_LoadTexture_RW(ren, rw, 1);
    if (!tex) {
        std::cerr << "AssetManager failed to load image (" << fileName << "): " << IMG_GetError() << std::endl;
        return nullptr;
    }

    textureCache[fileName] = tex;
    return tex;
}

TTF_Font* AssetManager::LoadFont(const std::string& fileName, int fontSize) {
    std::string key = GetFontKey(fileName, fontSize);
    if (fontCache.find(key) != fontCache.end()) {
        return fontCache[key];
    }

    std::vector<char> buffer = archiveManager.ExtractFile(fileName);
    if (buffer.empty()) return nullptr;

    fontBuffers[key] = std::move(buffer);
    SDL_RWops* rw = SDL_RWFromMem(fontBuffers[key].data(), fontBuffers[key].size());
    if (!rw) return nullptr;

    TTF_Font* font = TTF_OpenFontRW(rw, 1, fontSize * 2);  // temp
    if (!font) {
        std::cerr << "AssetManager failed to load font (" << fileName << "): " << TTF_GetError() << std::endl;
        return nullptr;
    }

    fontCache[key] = font;
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
    TTF_Font* olFont = TTF_OpenFontRW(rw, 1, sizeIt->second * 2);  // temp
    if (!olFont) return nullptr;

    TTF_SetFontOutline(olFont, outlineSize * 2);
    outlineFontCache[olKey] = olFont;
    return olFont;
}

Mix_Chunk* AssetManager::LoadSFX(const std::string& fileName) {
    if (sfxCache.find(fileName) != sfxCache.end()) {
        return sfxCache[fileName];
    }

    std::vector<char> buffer = archiveManager.ExtractFile(fileName);
    if (buffer.empty()) return nullptr;

    SDL_RWops* rw = SDL_RWFromMem(buffer.data(), buffer.size());
    if (!rw) return nullptr;

    Mix_Chunk* chunk = Mix_LoadWAV_RW(rw, 1);
    if (!chunk) {
        std::cerr << "AssetManager failed to load SFX (" << fileName << "): " << Mix_GetError() << std::endl;
        return nullptr;
    }

    sfxCache[fileName] = chunk;
    return chunk;
}

Mix_Music* AssetManager::LoadBGM(const std::string& fileName, std::vector<char>& outBuffer) {
    outBuffer = archiveManager.ExtractFile(fileName);
    if (outBuffer.empty()) return nullptr;

    SDL_RWops* rw = SDL_RWFromMem(outBuffer.data(), outBuffer.size());
    if (!rw) return nullptr;

    Mix_Music* music = Mix_LoadMUS_RW(rw, 1);
    if (!music) {
        std::cerr << "AssetManager failed to load BGM (" << fileName << "): " << Mix_GetError() << std::endl;
    }

    return music;
}

void AssetManager::CleanTextures() {
    for (auto& pair : textureCache) {
        if (pair.second) SDL_DestroyTexture(pair.second);
    }
    textureCache.clear();
}

void AssetManager::CleanFonts() {
    for (auto& pair : outlineFontCache) {
        if (pair.second) TTF_CloseFont(pair.second);
    }
    outlineFontCache.clear();
    for (auto& pair : fontCache) {
        if (pair.second) TTF_CloseFont(pair.second);
    }
    fontCache.clear();
    fontBuffers.clear();
    fontSizeByKey.clear();
    fontReverseMap.clear();
}

void AssetManager::CleanAudio() {
    for (auto& pair : sfxCache) {
        if (pair.second) Mix_FreeChunk(pair.second);
    }
    sfxCache.clear();
}

void AssetManager::CleanAll() {
    CleanTextures();
    CleanFonts();
    CleanAudio();
}
