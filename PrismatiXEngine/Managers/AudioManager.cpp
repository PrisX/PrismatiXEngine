#include "AudioManager.h"

#include <iostream>

#include "ArchiveManager.h"

AudioManager::AudioManager(ArchiveManager& archiveMgr) : archiveManager(archiveMgr) {}

void AudioManager::PlayBGM(const std::string& fileName, int loops) {
    if (currentBgm) {
        Mix_HaltMusic();
        Mix_FreeMusic(currentBgm);
        currentBgm = nullptr;
    }
    currentBgmBuffer.clear();

    currentBgmBuffer = archiveManager.ExtractFile(fileName);
    if (currentBgmBuffer.empty()) return;

    SDL_RWops* rw = SDL_RWFromMem(currentBgmBuffer.data(), currentBgmBuffer.size());
    currentBgm = Mix_LoadMUS_RW(rw, 1);

    if (currentBgm) {
        Mix_PlayMusic(currentBgm, loops);
    }
    else {
        std::cerr << "Failed to load BGM (" << fileName << "): " << Mix_GetError() << std::endl;
    }
}

void AudioManager::StopBGM() { Mix_HaltMusic(); }

Mix_Chunk* AudioManager::LoadSFX(const std::string& fileName) {
    if (sfxCache.find(fileName) != sfxCache.end()) {
        return sfxCache[fileName];
    }

    std::vector<char> buffer = archiveManager.ExtractFile(fileName);
    if (buffer.empty()) return nullptr;

    SDL_RWops* rw = SDL_RWFromMem(buffer.data(), buffer.size());
    Mix_Chunk* sfx = Mix_LoadWAV_RW(rw, 1);

    if (!sfx) {
        std::cerr << "Failed to load SFX (" << fileName << "): " << Mix_GetError() << std::endl;
    }
    else {
        sfxCache[fileName] = sfx;
    }
    return sfx;
}

void AudioManager::PlaySFX(const std::string& fileName, int loops) {
    Mix_Chunk* sfx = LoadSFX(fileName);
    if (sfx) {
        Mix_PlayChannel(-1, sfx, loops);  // -1 隨便選 channel
    }
}

void AudioManager::PlayVoice(const std::string& fileName) {
    Mix_Chunk* voice = LoadSFX(fileName);
    if (voice) {
        // 語音固定在 channel 0
        Mix_HaltChannel(0);
        Mix_PlayChannel(0, voice, 0);
    }
}

void AudioManager::StopVoice() { Mix_HaltChannel(0); }

void AudioManager::CleanCache() {
    if (currentBgm) {
        Mix_HaltMusic();
        Mix_FreeMusic(currentBgm);
        currentBgm = nullptr;
    }
    currentBgmBuffer.clear();

    for (auto& pair : sfxCache) {
        if (pair.second) Mix_FreeChunk(pair.second);
    }
    sfxCache.clear();
}