#include "AudioSystem.h"

#include <iostream>

#include "Core/Services/ResourceManager.h"

namespace PrismatiX::Systems {

AudioSystem::AudioSystem(PrismatiX::Services::ResourceManager& resMgr) : resourceManager(resMgr) {}

AudioSystem::~AudioSystem() { StopBGM(); }

void AudioSystem::PlayBGM(const std::string& fileName, int loops) {
    if (currentBgm) {
        Mix_HaltMusic();
        Mix_FreeMusic(currentBgm);
        currentBgm = nullptr;
    }
    currentBgmBuffer.clear();

    currentBgm = resourceManager.LoadBGM(fileName, currentBgmBuffer);

    if (currentBgm) {
        Mix_PlayMusic(currentBgm, loops);
    }
}

void AudioSystem::StopBGM() {
    Mix_HaltMusic();
    if (currentBgm) {
        Mix_FreeMusic(currentBgm);
        currentBgm = nullptr;
    }
    currentBgmBuffer.clear();
}

void AudioSystem::PlaySFX(const std::string& fileName, int loops) {
    Mix_Chunk* sfx = resourceManager.LoadSFX(fileName);
    if (sfx) {
        Mix_PlayChannel(-1, sfx, loops);
    }
}

void AudioSystem::PlayVoice(const std::string& fileName) {
    Mix_Chunk* voice = resourceManager.LoadSFX(fileName);
    if (voice) {
        // 為了不要被其他蓋過去所以用 channel 0
        Mix_HaltChannel(0);
        Mix_PlayChannel(0, voice, 0);
    }
}

void AudioSystem::StopVoice() { Mix_HaltChannel(0); }

void AudioSystem::SetBGMVolume(int volume) { Mix_VolumeMusic(volume); }

void AudioSystem::SetSFXVolume(int volume) { Mix_Volume(-1, volume); }

int AudioSystem::GetBGMVolume() const { return Mix_VolumeMusic(-1); }

int AudioSystem::GetSFXVolume() const { return Mix_Volume(-1, -1); }

} // namespace PrismatiX::Systems
