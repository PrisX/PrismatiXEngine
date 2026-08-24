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
    struct TrackState {
        std::string path;
        bool loop = false;
        bool playing = false;
        std::int64_t playbackFrame = 0;
    };
    struct RuntimeState {
        TrackState music;
        TrackState voice;
        TrackState ambience;
        std::string pendingMusicLoop;
        int mainVolume = 128;
        int musicVolume = 96;
        int voiceVolume = 128;
        int sfxVolume = 110;
        int ambienceVolume = 96;
    };
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
    void PlayAmbience(const std::string& path,bool loop=true,int fadeMs=0);
    void StopAmbience(int fadeMs=0);

    void SetBGMVolume(int volume);
    void SetSEVolume(int volume);
    void SetVoiceVolume(int volume);
    void SetAmbienceVolume(int volume);
    void SetMainVolume(int volume);
    [[nodiscard]] int BGMVolume() const { return m_bgmVolume; }
    [[nodiscard]] int SEVolume() const { return m_seVolume; }
    [[nodiscard]] int VoiceVolume() const { return m_voiceVolume; }
    [[nodiscard]] int AmbienceVolume() const { return m_ambienceVolume; }
    [[nodiscard]] int MainVolume() const { return m_mainVolume; }
    [[nodiscard]] int SampleRate() const { return m_sampleRate; }
    [[nodiscard]] RuntimeState CaptureState() const;
    bool RestoreState(const RuntimeState& state);
    bool RestoreMusicTrack(const TrackState& state, int volume);
    bool RestoreVoiceTrack(const TrackState& state, int volume);
    bool RestoreAmbienceTrack(const TrackState& state, int volume);
    bool SetMusicPaused(bool paused);
    bool SetVoicePaused(bool paused);
    bool SetAmbiencePaused(bool paused);

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
    void ApplyMixGains();
    static bool SetTrackPaused(MIX_Track* track, bool paused);

    io::VFS& m_vfs;
    bool m_initialized = false;
    int m_sampleRate = 48000;

    MIX_Mixer* m_mixer = nullptr;
    std::array<MIX_Track*, 2> m_bgmTracks{ nullptr, nullptr };
    std::array<MIX_Audio*, 2> m_bgmAudio{ nullptr, nullptr };
    int m_bgmActive = 0;
    MIX_Track* m_voiceTrack = nullptr;
    MIX_Track* m_ambienceTrack = nullptr;
    MIX_Audio* m_ambienceAudio = nullptr;
    std::vector<MIX_Track*> m_seTracks;
    std::vector<MIX_Audio*> m_seTrackAudio;  // parallel to m_seTracks
    std::unordered_map<std::string, AudioEntry> m_audio;
    std::string m_pendingLoop;  // body that follows a one-shot intro
    std::string m_bgmPath;
    bool m_bgmLoop = false;
    std::string m_voicePath;
    std::string m_ambiencePath;
    bool m_ambienceLoop = false;
    std::uint64_t m_useCounter = 0;
    AudioEntry m_voiceAudio;  // current voice line only (not cached)

    int m_bgmVolume = 96;
    int m_seVolume = 110;
    int m_voiceVolume = 128;
    int m_ambienceVolume = 96;
    int m_mainVolume = 128;
    bool m_voiceDucking = false;
};

}
