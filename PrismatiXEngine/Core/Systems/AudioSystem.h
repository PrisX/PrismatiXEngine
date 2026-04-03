#pragma once

#include <SDL_mixer.h>
#include <string>
#include <vector>

class ResourceManager;

class AudioSystem {
private:
    ResourceManager& resourceManager;
    Mix_Music* currentBgm = nullptr;
    std::vector<char> currentBgmBuffer;

public:
    AudioSystem(ResourceManager& resMgr);
    ~AudioSystem();

    // BGM
    void PlayBGM(const std::string& fileName, int loops = -1);
    void StopBGM();

    // SFX
    void PlaySFX(const std::string& fileName, int loops = 0);

    // Vocal
    void PlayVoice(const std::string& fileName);
    void StopVoice();

    // Volume
    void SetBGMVolume(int volume); // 0-128
    void SetSFXVolume(int volume); // 0-128
    int GetBGMVolume() const;
    int GetSFXVolume() const;
};
