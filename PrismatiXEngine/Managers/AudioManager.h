#pragma once
#include <SDL_mixer.h>

#include <iostream>
#include <string>
#include <unordered_map>

class AudioManager {
public:
    // BGM
    static void PlayBGM(const std::string& fileName, int loops = -1);  // 無限循環
    static void StopBGM();

    // SFX
    static void PlaySFX(const std::string& fileName, int loops = 0);

    // Vocal
    static void PlayVoice(const std::string& fileName);
    static void StopVoice();  // Let characters shut the fuck up

    // Cache
    static void CleanCache();

private:
    static std::vector<char> currentBgmBuffer;
    static Mix_Music* currentBgm;
    static std::unordered_map<std::string, Mix_Chunk*> sfxCache;

    static Mix_Music* LoadBGM(const std::string& fileName);
    static Mix_Chunk* LoadSFX(const std::string& fileName);
};