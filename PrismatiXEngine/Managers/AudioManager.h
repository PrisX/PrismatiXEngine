#pragma once
#include <SDL_mixer.h>

#include <iostream>
#include <string>
#include <unordered_map>

class ArchiveManager;

class AudioManager {
public:
    AudioManager(ArchiveManager& archiveMgr);
    ~AudioManager() = default;

    // BGM
    void PlayBGM(const std::string& fileName, int loops = -1);  // 無限循環
    void StopBGM();

    // SFX
    void PlaySFX(const std::string& fileName, int loops = 0);

    // Vocal
    void PlayVoice(const std::string& fileName);
    void StopVoice();  // Let characters shut the fuck up

    // Cache
    void CleanCache();

private:
    ArchiveManager& archiveManager;
    std::vector<char> currentBgmBuffer;
    Mix_Music* currentBgm = nullptr;
    std::unordered_map<std::string, Mix_Chunk*> sfxCache;

    Mix_Music* LoadBGM(const std::string& fileName);
    Mix_Chunk* LoadSFX(const std::string& fileName);
};