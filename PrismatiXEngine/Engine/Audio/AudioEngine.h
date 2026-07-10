#pragma once

#include "Engine/IO/VFS.h"

#include <array>
#include <cstdint>
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

    // With fadeMs > 0 while another BGM is playing this crossfades: the old
    // track fades out while the new one fades in on a second track.
    void PlayBGM(const std::string& path, bool loop = true, int fadeMs = 0);
    // Plays `introPath` once, then switches to `loopPath` looping forever
    // (handoff is polled in Update()).
    void PlayBGMWithIntro(const std::string& introPath, const std::string& loopPath,
                          int fadeMs = 0);
    void StopBGM(int fadeMs = 0);
    // Per-frame upkeep (intro -> loop handoff). Cheap.
    void Update();

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
        std::uint64_t lastUse = 0;
    };

    MIX_Audio* AcquireAudio(const std::string& path, bool predecode);
    // Returns the track and records the audio about to play on it.
    MIX_Track* AcquireSeTrack(MIX_Audio* audio);
    // Caps decoded-audio memory; long sessions otherwise grow without bound.
    void EvictAudio();

    io::VFS& m_vfs;
    bool m_initialized = false;
    int m_sampleRate = 48000;

    MIX_Mixer* m_mixer = nullptr;
    std::array<MIX_Track*, 2> m_bgmTracks{ nullptr, nullptr };
    std::array<MIX_Audio*, 2> m_bgmAudio{ nullptr, nullptr };
    int m_bgmActive = 0;
    MIX_Track* m_voiceTrack = nullptr;
    std::vector<MIX_Track*> m_seTracks;
    std::vector<MIX_Audio*> m_seTrackAudio;  // parallel to m_seTracks
    std::unordered_map<std::string, AudioEntry> m_audio;
    std::string m_pendingLoop;  // body that follows a one-shot intro
    std::uint64_t m_useCounter = 0;
    AudioEntry m_voiceAudio;  // current voice line only (not cached)

    int m_bgmVolume = 96;
    int m_seVolume = 110;
    int m_voiceVolume = 128;
};

}
