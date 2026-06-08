#pragma once

#include "Engine/IO/Vfs.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct MIX_Mixer;
struct MIX_Track;
struct MIX_Audio;

namespace px::audio {

class AudioEngine {
public:
    explicit AudioEngine(io::Vfs& vfs);
    ~AudioEngine();

    bool Init();
    void Shutdown();

    void PlayBgm(const std::string& path, bool loop = true, int fadeMs = 0);
    void StopBgm(int fadeMs = 0);

    void PlaySe(const std::string& path);
    void PlayVoice(const std::string& path);
    void StopVoice();

    void SetBgmVolume(int volume);
    void SetSeVolume(int volume);
    void SetVoiceVolume(int volume);
    [[nodiscard]] int BgmVolume() const { return m_bgmVolume; }
    [[nodiscard]] int SeVolume() const { return m_seVolume; }
    [[nodiscard]] int VoiceVolume() const { return m_voiceVolume; }

private:
    struct AudioEntry {
        MIX_Audio* audio = nullptr;
        std::shared_ptr<io::Bytes> bytes;
    };

    MIX_Audio* AcquireAudio(const std::string& path, bool predecode);
    MIX_Track* AcquireSeTrack();

    io::Vfs& m_vfs;
    bool m_initialized = false;
    int m_sampleRate = 48000;

    MIX_Mixer* m_mixer = nullptr;
    MIX_Track* m_bgmTrack = nullptr;
    MIX_Track* m_voiceTrack = nullptr;
    std::vector<MIX_Track*> m_seTracks;
    std::unordered_map<std::string, AudioEntry> m_audio;

    int m_bgmVolume = 96;
    int m_seVolume = 110;
    int m_voiceVolume = 128;
};

}
