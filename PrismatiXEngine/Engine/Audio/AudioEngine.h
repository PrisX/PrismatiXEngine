#pragma once

#include "Engine/IO/VFS.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct MIX_Mixer;
struct MIX_Track;
struct MIX_Audio;

namespace px::audio {

class AudioEngine {
public:
    enum class Bus : std::uint8_t {
        Music,
        SoundEffect,
        Voice,
        Ambience,
        BGM = Music,
        SE = SoundEffect,
    };
    using AudioBus = Bus;

    enum class PlaybackStatus : std::uint8_t {
        Invalid,
        Playing,
        Paused,
        Completed,
        Stopped,
        Failed,
    };

    enum class PlaybackPhase : std::uint8_t {
        Single,
        Intro,
        Loop,
    };

    enum class RestorePolicy : std::uint8_t {
        Persistent,
        Transient,
    };

    struct AudioHandle {
        std::uint64_t value = 0;
        [[nodiscard]] explicit operator bool() const { return value != 0; }
        friend bool operator==(AudioHandle, AudioHandle) = default;
    };
    using PlaybackHandle = AudioHandle;

    struct PlaybackRequest {
        Bus bus = Bus::SoundEffect;
        std::string path;
        bool loop = false;
        int fadeMs = 0;
        // When non-empty, path is a one-shot intro and loopPath becomes the
        // indefinitely looping body without changing the handle identity.
        std::string loopPath;
        // Level of this cue alone, 0..1, multiplied with its bus volume.
        float gain = 1.0f;
    };

    struct PlaybackState {
        AudioHandle handle;
        Bus bus = Bus::SoundEffect;
        PlaybackStatus status = PlaybackStatus::Invalid;
        PlaybackPhase phase = PlaybackPhase::Single;
        std::string path;
        std::string introPath;
        std::string loopPath;
        std::int64_t playbackFrame = 0;
        bool loop = false;
        [[nodiscard]] bool Terminal() const {
            return status == PlaybackStatus::Completed ||
                   status == PlaybackStatus::Stopped ||
                   status == PlaybackStatus::Failed;
        }
    };

    static constexpr std::size_t TerminalHistoryLimit = 64;
    static constexpr std::size_t kTerminalHistoryCapacity = TerminalHistoryLimit;
    using Completion = std::function<void(AudioHandle)>;

    struct TrackState {
        std::string path;
        bool loop = false;
        bool playing = false;
        std::int64_t playbackFrame = 0;
        bool paused = false;
        // Persistent buses carry their live identity through checkpoint
        // restore so an outstanding wait token continues observing the same
        // playback rather than a replacement handle.
        std::uint64_t handle = 0;
        // Cue level restored with the track; see PlaybackRequest::gain.
        float gain = 1.0f;
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

    // Native handle API. A valid handle is returned even when startup fails;
    // its status is Failed, allowing callers to distinguish a rejected play
    // from an absent/stale handle (Invalid).
    [[nodiscard]] AudioHandle Play(PlaybackRequest request);
    [[nodiscard]] AudioHandle PlayIntroLoop(Bus bus,
                                            const std::string& introPath,
                                            const std::string& loopPath,
                                            int fadeMs = 0, float gain = 1.0f);
    [[nodiscard]] std::optional<PlaybackState> Playback(AudioHandle handle) const;
    [[nodiscard]] PlaybackStatus Status(AudioHandle handle) const;
    [[nodiscard]] std::optional<PlaybackStatus> GetStatus(
        AudioHandle handle) const;
    [[nodiscard]] bool IsComplete(AudioHandle handle) const;
    [[nodiscard]] bool Active(AudioHandle handle) const;
    [[nodiscard]] bool Playing(AudioHandle handle) const;
    [[nodiscard]] bool Paused(AudioHandle handle) const;
    bool Pause(AudioHandle handle);
    bool Resume(AudioHandle handle);
    bool Stop(AudioHandle handle, int fadeMs = 0);
    bool Seek(AudioHandle handle, std::int64_t playbackFrame);
    // Natural completions are delivered once. Terminal Playback() state stays
    // queryable until the bounded history evicts the oldest terminal handle.
    [[nodiscard]] std::vector<AudioHandle> TakeCompletions();
    void SetCompletion(Completion completion) {
        m_completion = std::move(completion);
    }
    [[nodiscard]] std::size_t TerminalHistorySize() const {
        return m_terminalHistory.size();
    }
    [[nodiscard]] static constexpr RestorePolicy RestorePolicyFor(Bus bus) {
        // Music/voice/ambience are durable buses captured by RuntimeState.
        // Sound effects are overlapping transient cues and intentionally do
        // not replay during checkpoint restore.
        return bus == Bus::SoundEffect ? RestorePolicy::Transient
                                       : RestorePolicy::Persistent;
    }

    // With fadeMs > 0 while another BGM is playing this crossfades: the old
    // track fades out while the new one fades in on a second track.
    AudioHandle PlayBGM(const std::string& path, bool loop = true, int fadeMs = 0,
                        float gain = 1.0f);
    // Plays `introPath` once, then switches to `loopPath` looping forever
    // (handoff is polled in Update()).
    void PlayBGMWithIntro(const std::string& introPath, const std::string& loopPath,
                          int fadeMs = 0, float gain = 1.0f);
    void StopBGM(int fadeMs = 0);
    // Per-frame upkeep (intro -> loop handoff). Cheap.
    void Update();

    void PlaySE(const std::string& path, float gain = 1.0f);
    // Stops every playing sound effect, optionally fading it out.
    void StopSE(int fadeMs = 0);
    void PlayVoice(const std::string& path);
    void StopVoice();
    // Reads a voice or sound effect on a worker thread so the line that
    // plays it does not wait for the disk. Holds the last few requests.
    void Prefetch(const std::string& path);
    void PlayAmbience(const std::string& path,bool loop=true,int fadeMs=0,
                      float gain=1.0f);
    void StopAmbience(int fadeMs=0);

    void SetBGMVolume(int volume);
    void SetSEVolume(int volume);
    void SetVoiceVolume(int volume);
    void SetVoiceProfileVolume(std::string profile, int volume);
    // Voices whose folder is not a profile use the first profile whose file
    // name prefix matches, e.g. {"man_", "man"}.
    void SetVoiceProfileFallbacks(std::vector<std::pair<std::string, std::string>> prefixes);
    void SetVoiceDuckingEnabled(bool enabled);
    void SetAmbienceVolume(int volume);
    void SetMainVolume(int volume);
    [[nodiscard]] int BGMVolume() const { return m_bgmVolume; }
    [[nodiscard]] int SEVolume() const { return m_seVolume; }
    [[nodiscard]] int VoiceVolume() const { return m_voiceVolume; }
    [[nodiscard]] int AmbienceVolume() const { return m_ambienceVolume; }
    [[nodiscard]] int MainVolume() const { return m_mainVolume; }
    [[nodiscard]] int SampleRate() const { return m_sampleRate; }
    [[nodiscard]] RuntimeState CaptureState() const;
    [[nodiscard]] bool ValidateState(const RuntimeState& state,
                                     bool requireResources = true) const;
    // Decodes every referenced track and checks seek bounds without stopping
    // or replacing currently audible tracks.  Restore coordinators call this
    // during PrepareRestore so malformed media cannot fail after live state
    // mutation has started.
    bool PrepareRestoreState(const RuntimeState& state);
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

    struct PlaybackRecord {
        PlaybackState state;
        MIX_Track* track = nullptr;
    };

    MIX_Audio* AcquireAudio(const std::string& path, bool predecode);
    // Takes prefetched bytes when present, otherwise reads synchronously.
    std::optional<io::Bytes> ReadAudioFile(const std::string& path);
    // Returns the track and records the audio about to play on it.
    MIX_Track* AcquireSeTrack(MIX_Audio* audio);
    // Caps decoded-audio memory; long sessions otherwise grow without bound.
    void EvictAudio();
    void ApplyMixGains();
    static bool SetTrackPaused(MIX_Track* track, bool paused);
    bool StartMusic(const std::string& path, bool loop, int fadeMs, float gain);
    MIX_Track* StartSoundEffect(const std::string& path, float gain);
    bool StartVoice(const std::string& path);
    bool StartAmbience(const std::string& path, bool loop, int fadeMs, float gain);
    void StopMusicTracks(int fadeMs);
    void StopVoiceTrack();
    void StopAmbienceTrack(int fadeMs);
    void StopTransientSoundEffects();
    [[nodiscard]] AudioHandle NextHandle();
    [[nodiscard]] AudioHandle PlayWithHandle(PlaybackRequest request,
                                             AudioHandle restoredHandle);
    void ForgetTerminalHandle(AudioHandle handle);
    void Finish(AudioHandle handle, PlaybackStatus status);
    void FinishTrackPlayback(MIX_Track* track, PlaybackStatus status);
    [[nodiscard]] PlaybackRecord* Find(AudioHandle handle);
    [[nodiscard]] const PlaybackRecord* Find(AudioHandle handle) const;
    void TrimTerminalHistory();

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
    std::vector<float> m_seTrackGain;        // parallel to m_seTracks
    std::array<float, 2> m_bgmCueGain{1.0f, 1.0f};
    float m_ambienceCueGain = 1.0f;
    std::unordered_map<std::string, AudioEntry> m_audio;
    std::string m_pendingLoop;  // body that follows a one-shot intro
    std::string m_bgmPath;
    bool m_bgmLoop = false;
    std::string m_voicePath;
    std::string m_ambiencePath;
    bool m_ambienceLoop = false;
    std::uint64_t m_useCounter = 0;
    std::unordered_map<std::string, std::shared_future<std::optional<io::Bytes>>> m_prefetched;
    std::deque<std::string> m_prefetchOrder;
    // Evicted reads still in flight; dropping one would block until it ends.
    std::vector<std::shared_future<std::optional<io::Bytes>>> m_retiredPrefetches;
    AudioEntry m_voiceAudio;  // current voice line only (not cached)

    int m_bgmVolume = 96;
    int m_seVolume = 110;
    int m_voiceVolume = 128;
    std::unordered_map<std::string, int> m_voiceProfileVolumes;
    std::vector<std::pair<std::string, std::string>> m_voiceProfileFallbacks;
    bool m_voiceDuckingEnabled = true;
    int m_ambienceVolume = 96;
    int m_mainVolume = 128;
    bool m_voiceDucking = false;

    std::uint64_t m_nextHandle = 1;
    std::unordered_map<std::uint64_t,PlaybackRecord> m_playbacks;
    std::unordered_map<MIX_Track*,AudioHandle> m_trackPlaybacks;
    std::deque<AudioHandle> m_terminalHistory;
    std::vector<AudioHandle> m_completions;
    AudioHandle m_musicHandle;
    AudioHandle m_voiceHandle;
    AudioHandle m_ambienceHandle;
    Completion m_completion;
};

using AudioBus = AudioEngine::Bus;
using AudioHandle = AudioEngine::AudioHandle;
using PlaybackHandle = AudioEngine::PlaybackHandle;
using PlaybackStatus = AudioEngine::PlaybackStatus;
using PlaybackPhase = AudioEngine::PlaybackPhase;
using PlaybackRequest = AudioEngine::PlaybackRequest;
using PlaybackState = AudioEngine::PlaybackState;
using AudioRestorePolicy = AudioEngine::RestorePolicy;

}
