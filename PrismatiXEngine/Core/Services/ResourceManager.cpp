#include "ResourceManager.h"

#include <SDL2/SDL_image.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "Core/EngineConfig.h"
#include "Utils/Logger.h"

#ifndef PDX_SECRET_KEY
#define PDX_SECRET_KEY "PrismatiXDEFAULT"
#endif

namespace fs = std::filesystem;

namespace PrismatiX::Services {

ResourceManager::ResourceManager(SDL_Renderer* renderer, size_t texLimit, size_t fontLimit, size_t sfxLimit) : renderer(renderer) {
    textureCache = std::make_unique<PrismatiX::Utils::LRUCache<std::string, SDL_Texture*>>(texLimit, [](SDL_Texture* tex) {
        if (tex) SDL_DestroyTexture(tex);
    });

    sfxCache = std::make_unique<PrismatiX::Utils::LRUCache<std::string, Mix_Chunk*>>(sfxLimit, [](Mix_Chunk* chunk) {
        if (chunk) Mix_FreeChunk(chunk);
    });

    fontCache = std::make_unique<PrismatiX::Utils::LRUCache<std::string, FontAsset*>>(fontLimit, [this](FontAsset* asset) {
        if (asset) {
            if (asset->baseFont) {
                fontReverseMap.erase(asset->baseFont);
            }
            delete asset;
        }
    });

    ScanDirectory("Data");
    ScanDirectory("Engine");
    ScanDirectory("Scripts");
}

ResourceManager::~ResourceManager() { CleanAll(); }

void ResourceManager::ScanDirectory(const std::string& root) {
    if (!fs::exists(root) || !fs::is_directory(root)) return;
    PX_LOG_DEBUG("Scanning directory: {}", root);
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            std::string fileName = entry.path().filename().string();
            std::string fullPath = entry.path().string();
            std::replace(fullPath.begin(), fullPath.end(), '\\', '/');
            diskFileMap[fileName] = fullPath;
            std::string relativePath = fs::relative(entry.path(), root).string();
            std::replace(relativePath.begin(), relativePath.end(), '\\', '/');
            diskFileMap[relativePath] = fullPath;
        }
    }
}

bool ResourceManager::MountArchive(const std::string& archivePath) {
    if (!fs::exists(archivePath)) {
        PX_LOG_WARN("Archive not found: {}", archivePath);
        return true;
    }
    std::ifstream in(archivePath, std::ios::binary);
    if (!in) return false;
    char magic[4];
    in.read(magic, 4);
    if (strncmp(magic, "PDX!", 4) != 0) {
        PX_LOG_ERROR("Invalid archive format: {}", archivePath);
        return false;
    }
    uint32_t fileCount;
    in.read(reinterpret_cast<char*>(&fileCount), sizeof(fileCount));
    PX_LOG_INFO("Mounting archive: {} ({} files)", archivePath, fileCount);
    for (uint32_t i = 0; i < fileCount; ++i) {
        uint16_t nameLen;
        in.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
        std::string fileName(nameLen, '\0');
        in.read(&fileName[0], nameLen);
        PDXFileEntry entry;
        entry.archivePath = archivePath;
        in.read(reinterpret_cast<char*>(&entry.offset), sizeof(entry.offset));
        in.read(reinterpret_cast<char*>(&entry.size), sizeof(entry.size));
        globalFileTable[fileName] = entry;
    }
    return true;
}

std::vector<char> ResourceManager::ExtractFile(const std::string& fileName) {
    std::string normalizedName = fileName;
    std::replace(normalizedName.begin(), normalizedName.end(), '\\', '/');
    if (diskFileMap.count(normalizedName)) {
        std::string actualPath = diskFileMap[normalizedName];
        std::ifstream in(actualPath, std::ios::binary | std::ios::ate);
        if (in) {
            size_t size = in.tellg();
            std::vector<char> buffer(size);
            in.seekg(0, std::ios::beg);
            in.read(buffer.data(), size);
            return buffer;
        }
    }
    auto it = globalFileTable.find(normalizedName);
    if (it == globalFileTable.end()) {
        PX_LOG_DEBUG("File not found in archives: {}", normalizedName);
        return {};
    }
    PDXFileEntry entry = it->second;
    std::ifstream in(entry.archivePath, std::ios::binary);
    if (!in) return {};
    in.seekg(entry.offset, std::ios::beg);
    std::vector<char> buffer(entry.size);
    in.read(buffer.data(), entry.size);
    in.close();
    std::string_view key = PDX_SECRET_KEY;
    int keyLen = (int)key.length();
    if (keyLen > 0) {
        for (size_t i = 0; i < buffer.size(); i++) buffer[i] ^= key[i % keyLen];
    }
    return buffer;
}

std::string ResourceManager::LoadText(const std::string& path) {
    std::vector<char> buffer = ExtractFile(path);
    if (buffer.empty()) return "";
    return std::string(buffer.begin(), buffer.end());
}

std::string ResourceManager::GetFontKey(const std::string& fileName, int fontSize) { return fileName + "_" + std::to_string(fontSize); }

SDL_Texture* ResourceManager::LoadTexture(const std::string& fileName) {
    SDL_Texture* tex = nullptr;
    if (textureCache->Get(fileName, tex)) return tex;
    PX_LOG_TRACE("Loading texture: {}", fileName);
    std::vector<char> buffer = ExtractFile(fileName);
    if (buffer.empty()) return nullptr;
    SDL_RWops* rw = SDL_RWFromMem(buffer.data(), (int)buffer.size());
    if (!rw) return nullptr;
    tex = IMG_LoadTexture_RW(renderer, rw, 1);
    if (!tex) {
        PX_LOG_ERROR("Texture load failed ({}): {}", fileName, IMG_GetError());
        return nullptr;
    }
    textureCache->Put(fileName, tex);
    return tex;
}

TTF_Font* ResourceManager::LoadFont(const std::string& fileName, int fontSize) {
    std::string key = GetFontKey(fileName, fontSize);
    FontAsset* asset = nullptr;
    if (fontCache->Get(key, asset)) {
        return asset->baseFont;
    }

    PX_LOG_TRACE("Loading font: {} (Size: {})", fileName, fontSize);
    std::vector<char> buffer = ExtractFile(fileName);
    if (buffer.empty()) return nullptr;

    asset = new FontAsset();
    asset->buffer = std::move(buffer);
    asset->size = fontSize;

    SDL_RWops* rw = SDL_RWFromMem(asset->buffer.data(), (int)asset->buffer.size());
    if (!rw) {
        delete asset;
        return nullptr;
    }

    asset->baseFont = TTF_OpenFontRW(rw, 1, fontSize * EngineConfig::kFontOversample);
    if (!asset->baseFont) {
        PX_LOG_ERROR("Font load failed ({}): {}", fileName, TTF_GetError());
        delete asset;
        return nullptr;
    }

    fontCache->Put(key, asset);
    fontReverseMap[asset->baseFont] = key;
    return asset->baseFont;
}

TTF_Font* ResourceManager::GetOutlineFont(TTF_Font* baseFont, int outlineSize) {
    if (!baseFont || outlineSize < 0) return nullptr;

    auto keyIt = fontReverseMap.find(baseFont);
    if (keyIt == fontReverseMap.end()) return nullptr;

    FontAsset* asset = nullptr;
    if (!fontCache->Get(keyIt->second, asset)) return nullptr;

    if (asset->outlineFonts.size() <= (size_t)outlineSize) {
        asset->outlineFonts.resize(outlineSize + 1, nullptr);
    }

    if (asset->outlineFonts[outlineSize]) {
        return asset->outlineFonts[outlineSize];
    }

    SDL_RWops* rw = SDL_RWFromMem(asset->buffer.data(), (int)asset->buffer.size());
    if (!rw) return nullptr;

    TTF_Font* olFont = TTF_OpenFontRW(rw, 1, asset->size * EngineConfig::kFontOversample);
    if (!olFont) return nullptr;

    TTF_SetFontOutline(olFont, outlineSize * EngineConfig::kFontOversample);
    asset->outlineFonts[outlineSize] = olFont;

    return olFont;
}

Mix_Chunk* ResourceManager::LoadSFX(const std::string& fileName) {
    Mix_Chunk* chunk = nullptr;
    if (sfxCache->Get(fileName, chunk)) return chunk;
    PX_LOG_TRACE("Loading SFX: {}", fileName);
    std::vector<char> buffer = ExtractFile(fileName);
    if (buffer.empty()) return nullptr;
    SDL_RWops* rw = SDL_RWFromMem(buffer.data(), (int)buffer.size());
    if (!rw) return nullptr;
    chunk = Mix_LoadWAV_RW(rw, 1);
    if (!chunk) {
        PX_LOG_ERROR("SFX load failed ({}): {}", fileName, Mix_GetError());
        return nullptr;
    }
    sfxCache->Put(fileName, chunk);
    return chunk;
}

Mix_Music* ResourceManager::LoadBGM(const std::string& fileName, std::vector<char>& outBuffer) {
    PX_LOG_TRACE("Loading BGM: {}", fileName);
    outBuffer = ExtractFile(fileName);
    if (outBuffer.empty()) return nullptr;
    SDL_RWops* rw = SDL_RWFromMem(outBuffer.data(), (int)outBuffer.size());
    if (!rw) return nullptr;
    Mix_Music* music = Mix_LoadMUS_RW(rw, 1);
    if (!music) {
        PX_LOG_ERROR("BGM load failed ({}): {}", fileName, Mix_GetError());
    }
    return music;
}

void ResourceManager::CleanTextures() { textureCache->Clear(); }
void ResourceManager::CleanFonts() {
    fontCache->Clear();
    fontReverseMap.clear();
}
void ResourceManager::CleanAudio() { sfxCache->Clear(); }
void ResourceManager::CleanAll() {
    CleanTextures();
    CleanFonts();
    CleanAudio();
}

} // namespace PrismatiX::Services
