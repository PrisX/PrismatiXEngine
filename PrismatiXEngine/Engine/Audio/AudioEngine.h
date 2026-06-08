#pragma once

#include "Engine/IO/VFS.h"

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
    explicit AudioEngine(io::VFS& vfs);
    ~AudioEngine();

    bool Init();
    void Shutdown();

    void PlayBGM(const std::string& path, bool loop = true, int fadeMs = 0);
    void StopBGM(int fadeMs = 0);

    void PlaySE(const std::string& path);
    void PlayVoice(const std::string& path);
    void StopVoice();

    void SetBGMVolume(int volume);
    void SetSEVolume(int volume);
    void SetVoiceVolume(int volume);
    [[nodiscard]] int BGMVolume() const { return m_bgmVolume; }
    [[nodiscard]] int SEVolume() const { return m_seVolume; }
    [[nodiscard]] int VoiceVolume() const { return m_voiceVolume; }

private:
    struct AudioEntry {
        MIX_Audio* audio = nullptr;
        std::shared_ptr<io::Bytes> bytes;
    };

    MIX_Audio* AcquireAudio(const std::string& path, bool predecode);
    MIX_Track* AcquireSeTrack();

    io::VFS& m_vfs;
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
