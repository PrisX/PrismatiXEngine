#pragma once
#include <SDL_mixer.h>
#include <string>

namespace PrismatiX::Interfaces {

class IAudioPlayer {
public:
    virtual ~IAudioPlayer() = default;
    
    // Music/BGM
    virtual void PlayBGM(const std::string& path, int loops = -1, int fadeMs = 0) = 0;
    virtual void StopBGM(int fadeMs = 0) = 0;
    virtual void PauseBGM() = 0;
    virtual void ResumeBGM() = 0;
    virtual bool IsBGMPlaying() const = 0;
    
    // Sound Effects
    virtual void PlaySFX(const std::string& path, int loops = 0) = 0;
    virtual void StopAllSFX() = 0;
    
    // Voice
    virtual void PlayVoice(const std::string& path) = 0;
    virtual void StopVoice() = 0;
    virtual bool IsVoicePlaying() const = 0;
    
    // Volume control
    virtual void SetMasterVolume(float volume) = 0;
    virtual void SetBGMVolume(float volume) = 0;
    virtual void SetSFXVolume(float volume) = 0;
    virtual void SetVoiceVolume(float volume) = 0;
    
    virtual float GetMasterVolume() const = 0;
    virtual float GetBGMVolume() const = 0;
    virtual float GetSFXVolume() const = 0;
    virtual float GetVoiceVolume() const = 0;
};

} // namespace PrismatiX::Interfaces
