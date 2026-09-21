#include "Engine/Audio/AudioEngine.h"

#include "Engine/Support/Logger.h"

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include <algorithm>
#include <limits>

namespace px::audio {

namespace {
constexpr std::size_t kMaxSeTracks = 12;
constexpr std::size_t kMaxAudioEntries = 24;

float ToGain(int volume) {
    if (volume < 0) volume = 0;
    return static_cast<float>(volume) / 128.0f;
}
}

AudioEngine::AudioEngine(io::VFS& vfs) : m_vfs(vfs) {}

AudioEngine::~AudioEngine() {
    Shutdown();
}

bool AudioEngine::Init() {
    if (m_initialized) {
        return true;
    }
    if (!MIX_Init()) {
        PX_LOG_ERROR("MIX_Init failed: {}", SDL_GetError());
        return false;
    }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = 48000;
    // A memory mixer advances only when MIX_Generate() is called. Runtime
    // playback needs the device-backed mixer so positions, natural completion,
    // intro handoff, and voice-aware waits progress with the audio clock.
    m_mixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (!m_mixer) {
        PX_LOG_ERROR("MIX_CreateMixerDevice failed: {}", SDL_GetError());
        MIX_Quit();
        return false;
    }
    SDL_AudioSpec actual{};
    if (MIX_GetMixerFormat(m_mixer, &actual)) {
        m_sampleRate = actual.freq;
    }
    for (MIX_Track*& track : m_bgmTracks) {
        track = MIX_CreateTrack(m_mixer);
    }
    m_voiceTrack = MIX_CreateTrack(m_mixer);
    m_ambienceTrack=MIX_CreateTrack(m_mixer);
    m_initialized = true;
    PX_LOG_INFO("AudioEngine initialized ({} Hz).", m_sampleRate);
    return true;
}

void AudioEngine::Shutdown() {
    if (!m_initialized) {
        return;
    }
    std::vector<AudioHandle> active;
    active.reserve(m_playbacks.size());
    for (const auto& [id, record] : m_playbacks) {
        if (!record.state.Terminal()) active.push_back(AudioHandle{id});
    }
    for (const AudioHandle handle : active) Finish(handle, PlaybackStatus::Stopped);
    m_trackPlaybacks.clear();
    for (MIX_Track*& track : m_bgmTracks) {
        if (track) MIX_DestroyTrack(track), track = nullptr;
    }
    m_bgmAudio = { nullptr, nullptr };
    if (m_voiceTrack) MIX_DestroyTrack(m_voiceTrack), m_voiceTrack = nullptr;
    if (m_ambienceTrack) {
        MIX_DestroyTrack(m_ambienceTrack);
        m_ambienceTrack = nullptr;
    }
    m_ambienceAudio = nullptr;
    if (m_voiceAudio.audio) {
        MIX_DestroyAudio(m_voiceAudio.audio);
        m_voiceAudio = {};
    }
    for (MIX_Track* t : m_seTracks) {
        MIX_DestroyTrack(t);
    }
    m_seTracks.clear();
    m_seTrackAudio.clear();
    for (auto& [path, entry] : m_audio) {
        if (entry.audio) {
            MIX_DestroyAudio(entry.audio);
        }
    }
    m_audio.clear();
    if (m_mixer) MIX_DestroyMixer(m_mixer), m_mixer = nullptr;
    MIX_Quit();
    m_initialized = false;
}

MIX_Audio* AudioEngine::AcquireAudio(const std::string& path, bool predecode) {
    ++m_useCounter;
    if (auto it = m_audio.find(path); it != m_audio.end()) {
        it->second.lastUse = m_useCounter;
        return it->second.audio;
    }
    auto bytes = m_vfs.Read(path);
    if (!bytes) {
        PX_LOG_WARN("AudioEngine: audio not found '{}'", path);
        m_audio[path] = AudioEntry{ nullptr, nullptr, m_useCounter };
        return nullptr;
    }
    auto held = std::make_shared<io::Bytes>(std::move(*bytes));
    SDL_IOStream* io = SDL_IOFromConstMem(held->data(), held->size());
    MIX_Audio* audio = io ? MIX_LoadAudio_IO(m_mixer, io, predecode, /*closeio=*/true) : nullptr;
    if (!audio) {
        PX_LOG_WARN("AudioEngine: failed to load audio '{}': {}", path, SDL_GetError());
        m_audio[path] = AudioEntry{ nullptr, nullptr, m_useCounter };
        return nullptr;
    }
    m_audio[path] = AudioEntry{ audio, held, m_useCounter };
    EvictAudio();
    return audio;
}

void AudioEngine::EvictAudio() {
    if (m_audio.size() <= kMaxAudioEntries) {
        return;
    }
    std::vector<std::pair<std::uint64_t, std::string>> order;
    order.reserve(m_audio.size());
    for (const auto& [path, entry] : m_audio) {
        order.emplace_back(entry.lastUse, path);
    }
    std::sort(order.begin(), order.end());

    for (const auto& [lastUse, path] : order) {
        if (m_audio.size() <= kMaxAudioEntries) {
            break;
        }
        auto it = m_audio.find(path);
        if (it == m_audio.end()) {
            continue;
        }
        MIX_Audio* audio = it->second.audio;
        if (audio && (audio == m_bgmAudio[0] || audio == m_bgmAudio[1] || audio==m_ambienceAudio)) {
            continue;  // a BGM track may still reference it
        }
        bool busy = false;
        if (audio) {
            for (std::size_t i = 0; i < m_seTracks.size(); ++i) {
                if (m_seTrackAudio[i] != audio) {
                    continue;
                }
                if (MIX_TrackPlaying(m_seTracks[i])) {
                    busy = true;
                    break;
                }
                MIX_SetTrackAudio(m_seTracks[i], nullptr);
                m_seTrackAudio[i] = nullptr;
            }
        }
        if (busy) {
            continue;
        }
        if (audio) {
            MIX_DestroyAudio(audio);
        }
        m_audio.erase(it);
    }
}

MIX_Track* AudioEngine::AcquireSeTrack(MIX_Audio* audio) {
    for (std::size_t i = 0; i < m_seTracks.size(); ++i) {
        if (!MIX_TrackPlaying(m_seTracks[i])) {
            m_seTrackAudio[i] = audio;
            return m_seTracks[i];
        }
    }
    if (m_seTracks.size() < kMaxSeTracks) {
        MIX_Track* t = MIX_CreateTrack(m_mixer);
        if (t) {
            m_seTracks.push_back(t);
            m_seTrackAudio.push_back(audio);
        }
        return t;
    }
    m_seTrackAudio.front() = audio;
    return m_seTracks.front();
}

AudioEngine::AudioHandle AudioEngine::NextHandle() {
    if (m_nextHandle == 0) ++m_nextHandle;
    return AudioHandle{m_nextHandle++};
}

void AudioEngine::ForgetTerminalHandle(const AudioHandle handle) {
    const auto found = m_playbacks.find(handle.value);
    if (found != m_playbacks.end() && found->second.state.Terminal())
        m_playbacks.erase(found);
    std::erase(m_terminalHistory, handle);
    std::erase(m_completions, handle);
}

AudioEngine::PlaybackRecord* AudioEngine::Find(const AudioHandle handle) {
    const auto found = m_playbacks.find(handle.value);
    return found == m_playbacks.end() ? nullptr : &found->second;
}

const AudioEngine::PlaybackRecord* AudioEngine::Find(
    const AudioHandle handle) const {
    const auto found = m_playbacks.find(handle.value);
    return found == m_playbacks.end() ? nullptr : &found->second;
}

void AudioEngine::TrimTerminalHistory() {
    while (m_terminalHistory.size() > kTerminalHistoryCapacity) {
        const AudioHandle oldest = m_terminalHistory.front();
        m_terminalHistory.pop_front();
        const auto found = m_playbacks.find(oldest.value);
        if (found != m_playbacks.end() && found->second.state.Terminal())
            m_playbacks.erase(found);
    }
}

void AudioEngine::Finish(const AudioHandle handle,
                         const PlaybackStatus status) {
    PlaybackRecord* record = Find(handle);
    if (!record || record->state.Terminal()) return;
    record->state.status = status;
    if (record->track) {
        const auto track = m_trackPlaybacks.find(record->track);
        if (track != m_trackPlaybacks.end() && track->second == handle)
            m_trackPlaybacks.erase(track);
    }
    if (m_musicHandle == handle) {
        m_musicHandle = {};
        m_bgmPath.clear();
        m_pendingLoop.clear();
    }
    if (m_voiceHandle == handle) {
        m_voiceHandle = {};
        m_voicePath.clear();
    }
    if (m_ambienceHandle == handle) {
        m_ambienceHandle = {};
        m_ambiencePath.clear();
    }
    if (status == PlaybackStatus::Completed) {
        if (m_completions.size() == kTerminalHistoryCapacity)
            m_completions.erase(m_completions.begin());
        m_completions.push_back(handle);
        if (m_completion) m_completion(handle);
    }
    m_terminalHistory.push_back(handle);
    TrimTerminalHistory();
}

void AudioEngine::FinishTrackPlayback(MIX_Track* track,
                                      const PlaybackStatus status) {
    const auto found = m_trackPlaybacks.find(track);
    if (found != m_trackPlaybacks.end()) Finish(found->second, status);
}

bool AudioEngine::StartMusic(const std::string& path, const bool loop,
                             const int fadeMs) {
    if (!m_initialized) return false;
    MIX_Track* current = m_bgmTracks[m_bgmActive];
    const bool crossfade = fadeMs > 0 && current && MIX_TrackPlaying(current);
    const int next = crossfade ? 1 - m_bgmActive : m_bgmActive;
    MIX_Track* track = m_bgmTracks[next];
    if (!track) return false;

    MIX_StopTrack(track, 0);
    MIX_Audio* audio = AcquireAudio(path, /*predecode=*/false);
    if (!audio || !MIX_SetTrackAudio(track, audio)) return false;
    m_bgmAudio[next] = audio;
    ApplyMixGains();

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, MIX_PROP_PLAY_LOOPS_NUMBER, loop ? -1 : 0);
    if (fadeMs > 0) {
        SDL_SetNumberProperty(props, MIX_PROP_PLAY_FADE_IN_MILLISECONDS_NUMBER, fadeMs);
    }
    const bool started = MIX_PlayTrack(track, props);
    SDL_DestroyProperties(props);
    if (!started) return false;
    m_bgmPath = path;
    m_bgmLoop = loop;

    if (crossfade) {
        MIX_StopTrack(current, MIX_MSToFrames(m_sampleRate, fadeMs));
        m_bgmActive = next;
    }
    return true;
}

MIX_Track* AudioEngine::StartSoundEffect(const std::string& path) {
    if (!m_initialized) return nullptr;
    MIX_Audio* audio = AcquireAudio(path, /*predecode=*/true);
    if (!audio) return nullptr;
    MIX_Track* track = AcquireSeTrack(audio);
    if (!track) return nullptr;
    const bool wasPlaying = MIX_TrackPlaying(track);
    FinishTrackPlayback(track, wasPlaying ? PlaybackStatus::Stopped
                                          : PlaybackStatus::Completed);
    MIX_StopTrack(track, 0);
    if (!MIX_SetTrackAudio(track, audio)) return nullptr;
    ApplyMixGains();
    return MIX_PlayTrack(track, 0) ? track : nullptr;
}

bool AudioEngine::StartVoice(const std::string& path) {
    if (!m_initialized || !m_voiceTrack) return false;
    // Voice lines are unique per dialogue line. Caching every line would grow
    // without bound, so each voice owns only the bytes used by the live track.
    auto bytes = m_vfs.Read(path);
    if (!bytes) {
        PX_LOG_WARN("AudioEngine: audio not found '{}'", path);
        return false;
    }
    MIX_StopTrack(m_voiceTrack, 0);
    if (m_voiceAudio.audio) {
        MIX_SetTrackAudio(m_voiceTrack, nullptr);
        MIX_DestroyAudio(m_voiceAudio.audio);
        m_voiceAudio = {};
    }
    auto held = std::make_shared<io::Bytes>(std::move(*bytes));
    SDL_IOStream* io = SDL_IOFromConstMem(held->data(), held->size());
    MIX_Audio* audio = io ? MIX_LoadAudio_IO(
                                m_mixer, io, /*predecode=*/true,
                                /*closeio=*/true)
                          : nullptr;
    if (!audio) {
        PX_LOG_WARN("AudioEngine: failed to load voice '{}': {}", path,
                    SDL_GetError());
        return false;
    }
    m_voiceAudio = AudioEntry{audio, held, 0};
    if (!MIX_SetTrackAudio(m_voiceTrack, audio)) return false;
    ApplyMixGains();
    if (!MIX_PlayTrack(m_voiceTrack, 0)) return false;
    m_voicePath = path;
    return true;
}

bool AudioEngine::StartAmbience(const std::string& path, const bool loop,
                                const int fadeMs) {
    if (!m_initialized || !m_ambienceTrack) return false;
    MIX_Audio* audio = AcquireAudio(path, false);
    if (!audio || !MIX_SetTrackAudio(m_ambienceTrack, audio)) return false;
    m_ambienceAudio = audio;
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, MIX_PROP_PLAY_LOOPS_NUMBER, loop ? -1 : 0);
    if (fadeMs > 0)
        SDL_SetNumberProperty(props,
                              MIX_PROP_PLAY_FADE_IN_MILLISECONDS_NUMBER,
                              fadeMs);
    ApplyMixGains();
    const bool started = MIX_PlayTrack(m_ambienceTrack, props);
    SDL_DestroyProperties(props);
    if (!started) return false;
    m_ambiencePath = path;
    m_ambienceLoop = loop;
    return true;
}

void AudioEngine::StopMusicTracks(const int fadeMs) {
    if (!m_initialized) return;
    const Sint64 frames = fadeMs > 0 ? MIX_MSToFrames(m_sampleRate, fadeMs) : 0;
    for (MIX_Track* track : m_bgmTracks)
        if (track) MIX_StopTrack(track, frames);
    m_bgmPath.clear();
    m_pendingLoop.clear();
}

void AudioEngine::StopVoiceTrack() {
    if (m_initialized && m_voiceTrack) MIX_StopTrack(m_voiceTrack, 0);
    m_voicePath.clear();
}

void AudioEngine::StopAmbienceTrack(const int fadeMs) {
    if (m_initialized && m_ambienceTrack)
        MIX_StopTrack(m_ambienceTrack,
                      fadeMs > 0 ? MIX_MSToFrames(m_sampleRate, fadeMs) : 0);
    m_ambiencePath.clear();
}

void AudioEngine::StopTransientSoundEffects() {
    for (MIX_Track* track : m_seTracks) {
        if (!track) continue;
        MIX_StopTrack(track, 0);
        FinishTrackPlayback(track, PlaybackStatus::Stopped);
    }
}

AudioEngine::AudioHandle AudioEngine::Play(PlaybackRequest request) {
    return PlayWithHandle(std::move(request), {});
}

AudioEngine::AudioHandle AudioEngine::PlayWithHandle(
    PlaybackRequest request, const AudioHandle restoredHandle) {
    AudioHandle handle = restoredHandle;
    if (handle) {
        ForgetTerminalHandle(handle);
        // A restored handle must never make a later fresh handle go backward.
        if (handle.value >= m_nextHandle) m_nextHandle = handle.value + 1;
    } else {
        handle = NextHandle();
    }
    PlaybackRecord record;
    record.state.handle = handle;
    record.state.bus = request.bus;
    record.state.status = PlaybackStatus::Playing;
    record.state.path = request.path;
    record.state.loop = request.loop;
    if (!request.loopPath.empty()) {
        record.state.phase = PlaybackPhase::Intro;
        record.state.introPath = request.path;
        record.state.loopPath = request.loopPath;
        record.state.loop = false;
    }
    m_playbacks.emplace(handle.value, std::move(record));

    if (!m_initialized || request.path.empty() ||
        (!request.loopPath.empty() && request.bus != Bus::Music) ||
        (!request.loopPath.empty() && request.loopPath == request.path)) {
        Finish(handle, PlaybackStatus::Failed);
        return handle;
    }

    MIX_Track* track = nullptr;
    switch (request.bus) {
        case Bus::Music:
            if (m_musicHandle) Finish(m_musicHandle, PlaybackStatus::Stopped);
            m_pendingLoop.clear();
            if (StartMusic(request.path, request.loopPath.empty() && request.loop,
                           std::max(0, request.fadeMs))) {
                track = m_bgmTracks[m_bgmActive];
                m_musicHandle = handle;
                m_pendingLoop = request.loopPath;
            }
            break;
        case Bus::SoundEffect:
            track = StartSoundEffect(request.path);
            break;
        case Bus::Voice:
            if (m_voiceHandle) Finish(m_voiceHandle, PlaybackStatus::Stopped);
            if (StartVoice(request.path)) {
                track = m_voiceTrack;
                m_voiceHandle = handle;
            }
            break;
        case Bus::Ambience:
            if (m_ambienceHandle)
                Finish(m_ambienceHandle, PlaybackStatus::Stopped);
            if (StartAmbience(request.path, request.loop,
                              std::max(0, request.fadeMs))) {
                track = m_ambienceTrack;
                m_ambienceHandle = handle;
            }
            break;
    }
    if (!track) {
        if (request.bus == Bus::Music) StopMusicTracks(0);
        else if (request.bus == Bus::Voice) StopVoiceTrack();
        else if (request.bus == Bus::Ambience) StopAmbienceTrack(0);
        Finish(handle, PlaybackStatus::Failed);
        return handle;
    }
    PlaybackRecord* started = Find(handle);
    started->track = track;
    m_trackPlaybacks[track] = handle;
    return handle;
}

AudioEngine::AudioHandle AudioEngine::PlayIntroLoop(
    const Bus bus, const std::string& introPath, const std::string& loopPath,
    const int fadeMs) {
    return Play({.bus=bus,
                 .path=introPath,
                 .loop=false,
                 .fadeMs=fadeMs,
                 .loopPath=loopPath});
}

std::optional<AudioEngine::PlaybackState> AudioEngine::Playback(
    const AudioHandle handle) const {
    const PlaybackRecord* record = Find(handle);
    if (!record) return std::nullopt;
    PlaybackState state = record->state;
    if (!state.Terminal() && record->track) {
        state.playbackFrame = std::max<Sint64>(
            0, MIX_GetTrackPlaybackPosition(record->track));
    }
    return state;
}

AudioEngine::PlaybackStatus AudioEngine::Status(
    const AudioHandle handle) const {
    const PlaybackRecord* record = Find(handle);
    return record ? record->state.status : PlaybackStatus::Invalid;
}

std::optional<AudioEngine::PlaybackStatus> AudioEngine::GetStatus(
    const AudioHandle handle) const {
    const PlaybackRecord* record = Find(handle);
    if (!record) return std::nullopt;
    return record->state.status;
}

bool AudioEngine::IsComplete(const AudioHandle handle) const {
    return Status(handle) == PlaybackStatus::Completed;
}

bool AudioEngine::Active(const AudioHandle handle) const {
    const PlaybackStatus status = Status(handle);
    return status == PlaybackStatus::Playing || status == PlaybackStatus::Paused;
}

bool AudioEngine::Playing(const AudioHandle handle) const {
    return Status(handle) == PlaybackStatus::Playing;
}

bool AudioEngine::Paused(const AudioHandle handle) const {
    return Status(handle) == PlaybackStatus::Paused;
}

bool AudioEngine::Pause(const AudioHandle handle) {
    PlaybackRecord* record = Find(handle);
    if (!record || record->state.status != PlaybackStatus::Playing ||
        !record->track)
        return false;
    if (!MIX_TrackPlaying(record->track) &&
        !MIX_TrackPaused(record->track)) {
        Finish(handle, PlaybackStatus::Completed);
        return false;
    }
    if (!SetTrackPaused(record->track, true)) return false;
    record->state.status = PlaybackStatus::Paused;
    return true;
}

bool AudioEngine::Resume(const AudioHandle handle) {
    PlaybackRecord* record = Find(handle);
    if (!record || record->state.status != PlaybackStatus::Paused ||
        !SetTrackPaused(record->track, false))
        return false;
    record->state.status = PlaybackStatus::Playing;
    return true;
}

bool AudioEngine::Stop(const AudioHandle handle, const int fadeMs) {
    PlaybackRecord* record = Find(handle);
    if (!record || record->state.Terminal()) return false;
    switch (record->state.bus) {
        case Bus::Music: StopMusicTracks(std::max(0, fadeMs)); break;
        case Bus::Voice: StopVoiceTrack(); break;
        case Bus::Ambience: StopAmbienceTrack(std::max(0, fadeMs)); break;
        case Bus::SoundEffect:
            if (record->track)
                MIX_StopTrack(record->track,
                              fadeMs > 0
                                  ? MIX_MSToFrames(m_sampleRate, fadeMs)
                                  : 0);
            break;
    }
    Finish(handle, PlaybackStatus::Stopped);
    return true;
}

bool AudioEngine::Seek(const AudioHandle handle,
                       const std::int64_t playbackFrame) {
    PlaybackRecord* record = Find(handle);
    if (!record || record->state.Terminal() || !record->track ||
        playbackFrame < 0 ||
        !MIX_SetTrackPlaybackPosition(record->track, playbackFrame))
        return false;
    record->state.playbackFrame = playbackFrame;
    return true;
}

std::vector<AudioEngine::AudioHandle> AudioEngine::TakeCompletions() {
    std::vector<AudioHandle> completed = std::move(m_completions);
    m_completions.clear();
    return completed;
}

void AudioEngine::PlayBGM(const std::string& path, const bool loop,
                          const int fadeMs) {
    (void)Play({.bus=Bus::Music,
                .path=path,
                .loop=loop,
                .fadeMs=fadeMs});
}

void AudioEngine::PlayBGMWithIntro(const std::string& introPath,
                                   const std::string& loopPath,
                                   const int fadeMs) {
    (void)PlayIntroLoop(Bus::Music, introPath, loopPath, fadeMs);
}

void AudioEngine::PlaySE(const std::string& path) {
    (void)Play({.bus=Bus::SoundEffect, .path=path});
}

void AudioEngine::PlayVoice(const std::string& path) {
    (void)Play({.bus=Bus::Voice, .path=path});
}

void AudioEngine::PlayAmbience(const std::string& path, const bool loop,
                               const int fadeMs) {
    (void)Play({.bus=Bus::Ambience,
                .path=path,
                .loop=loop,
                .fadeMs=fadeMs});
}

void AudioEngine::StopBGM(const int fadeMs) {
    if (m_musicHandle) (void)Stop(m_musicHandle, fadeMs);
    else StopMusicTracks(fadeMs);
}

void AudioEngine::StopVoice() {
    if (m_voiceHandle) (void)Stop(m_voiceHandle);
    else StopVoiceTrack();
}

void AudioEngine::StopAmbience(const int fadeMs) {
    if (m_ambienceHandle) (void)Stop(m_ambienceHandle, fadeMs);
    else StopAmbienceTrack(fadeMs);
}

void AudioEngine::Update() {
    if (!m_initialized) return;
    const bool duck = m_voiceTrack && MIX_TrackPlaying(m_voiceTrack);
    if (duck != m_voiceDucking) {
        m_voiceDucking = duck;
        ApplyMixGains();
    }

    if (!m_pendingLoop.empty() && m_musicHandle) {
        PlaybackRecord* music = Find(m_musicHandle);
        MIX_Track* track = m_bgmTracks[m_bgmActive];
        if (music && music->state.status == PlaybackStatus::Playing && track &&
            !MIX_TrackPlaying(track) && !MIX_TrackPaused(track)) {
            const std::string loop = m_pendingLoop;
            m_pendingLoop.clear();
            if (StartMusic(loop, /*loop=*/true, 0)) {
                m_trackPlaybacks.erase(music->track);
                music->track = m_bgmTracks[m_bgmActive];
                music->state.path = loop;
                music->state.phase = PlaybackPhase::Loop;
                music->state.loop = true;
                music->state.playbackFrame = 0;
                m_trackPlaybacks[music->track] = m_musicHandle;
            } else {
                Finish(m_musicHandle, PlaybackStatus::Failed);
            }
        }
    }

    std::vector<AudioHandle> completed;
    completed.reserve(m_trackPlaybacks.size());
    for (const auto& [track, handle] : m_trackPlaybacks) {
        const PlaybackRecord* record = Find(handle);
        if (record && record->state.status == PlaybackStatus::Playing &&
            track && !MIX_TrackPlaying(track) && !MIX_TrackPaused(track))
            completed.push_back(handle);
    }
    for (const AudioHandle handle : completed)
        Finish(handle, PlaybackStatus::Completed);
}

AudioEngine::RuntimeState AudioEngine::CaptureState() const {
    const auto trackState = [](MIX_Track* track, const std::string& path,
                               const bool loop, const AudioHandle handle) {
        TrackState state{.path=path,.loop=loop};
        if (track && !path.empty()) {
            state.paused = MIX_TrackPaused(track);
            state.playing = state.paused || MIX_TrackPlaying(track);
            state.playbackFrame = std::max<Sint64>(0, MIX_GetTrackPlaybackPosition(track));
            state.handle = handle.value;
        }
        return state;
    };
    RuntimeState state;
    state.music = trackState(m_bgmTracks[m_bgmActive], m_bgmPath, m_bgmLoop,
                             m_musicHandle);
    state.voice = trackState(m_voiceTrack, m_voicePath, false, m_voiceHandle);
    state.ambience = trackState(m_ambienceTrack, m_ambiencePath,
                                m_ambienceLoop, m_ambienceHandle);
    state.pendingMusicLoop = m_pendingLoop;
    state.mainVolume = m_mainVolume;
    state.musicVolume = m_bgmVolume;
    state.voiceVolume = m_voiceVolume;
    state.sfxVolume = m_seVolume;
    state.ambienceVolume = m_ambienceVolume;
    return state;
}

bool AudioEngine::ValidateState(const RuntimeState& state,
                                const bool requireResources) const {
    if (state.mainVolume < 0 || state.mainVolume > 128 ||
        state.musicVolume < 0 || state.musicVolume > 128 ||
        state.voiceVolume < 0 || state.voiceVolume > 128 ||
        state.sfxVolume < 0 || state.sfxVolume > 128 ||
        state.ambienceVolume < 0 || state.ambienceVolume > 128 ||
        state.music.playbackFrame < 0 || state.voice.playbackFrame < 0 ||
        state.ambience.playbackFrame < 0 ||
        state.music.handle == (std::numeric_limits<std::uint64_t>::max)() ||
        state.voice.handle == (std::numeric_limits<std::uint64_t>::max)() ||
        state.ambience.handle == (std::numeric_limits<std::uint64_t>::max)() ||
        (state.music.paused && !state.music.playing) ||
        (state.voice.paused && !state.voice.playing) ||
        (state.ambience.paused && !state.ambience.playing) ||
        (state.music.handle != 0 &&
         (state.music.handle == state.voice.handle ||
          state.music.handle == state.ambience.handle)) ||
        (state.voice.handle != 0 &&
         state.voice.handle == state.ambience.handle)) return false;
    if (!requireResources) return true;
    for (const auto* track : {&state.music, &state.voice, &state.ambience}) {
        if (track->playing &&
            (track->path.empty() || !m_vfs.Exists(track->path))) return false;
    }
    if (!state.pendingMusicLoop.empty() &&
        !m_vfs.Exists(state.pendingMusicLoop)) return false;
    return true;
}

bool AudioEngine::PrepareRestoreState(const RuntimeState& state) {
    if (!ValidateState(state)) return false;
    // Headless validation has no mixer with which to decode.  The native
    // Player always initializes AudioEngine before constructing a session.
    if (!m_initialized) return true;
    for (const auto* track : {&state.music, &state.voice, &state.ambience}) {
        if (!track->playing) continue;
        MIX_Audio* audio = AcquireAudio(track->path, /*predecode=*/true);
        if (!audio) return false;
        const Sint64 duration = MIX_GetAudioDuration(audio);
        if (duration >= 0 && track->playbackFrame > duration) return false;
    }
    if (!state.pendingMusicLoop.empty() &&
        !AcquireAudio(state.pendingMusicLoop, /*predecode=*/true)) return false;
    return true;
}

bool AudioEngine::RestoreState(const RuntimeState& state) {
    if (!PrepareRestoreState(state)) return false;
    if (!m_initialized &&
        (state.music.playing || state.voice.playing || state.ambience.playing))
        return false;
    SetMainVolume(state.mainVolume);
    SetBGMVolume(state.musicVolume);
    SetVoiceVolume(state.voiceVolume);
    SetSEVolume(state.sfxVolume);
    SetAmbienceVolume(state.ambienceVolume);
    StopBGM(0); StopVoice(); StopAmbience(0);
    // SE is explicitly transient: restore silences in-flight cues and never
    // replays them from checkpoint data.
    StopTransientSoundEffects();
    if (state.music.playing && !state.music.path.empty()) {
        const AudioHandle handle = PlayWithHandle(
            {.bus=Bus::Music,
             .path=state.music.path,
             .loop=state.music.loop,
             .loopPath=state.pendingMusicLoop},
            AudioHandle{state.music.handle});
        if (!Active(handle) || !Seek(handle, state.music.playbackFrame))
            return false;
        if (state.music.paused && !SetMusicPaused(true)) return false;
    }
    if (state.voice.playing && !state.voice.path.empty()) {
        const AudioHandle handle = PlayWithHandle(
            {.bus=Bus::Voice,.path=state.voice.path},
            AudioHandle{state.voice.handle});
        if (!Active(handle) || !Seek(handle, state.voice.playbackFrame))
            return false;
        if (state.voice.paused && !SetVoicePaused(true)) return false;
    }
    if (state.ambience.playing && !state.ambience.path.empty()) {
        const AudioHandle handle = PlayWithHandle(
            {.bus=Bus::Ambience,
             .path=state.ambience.path,
             .loop=state.ambience.loop},
            AudioHandle{state.ambience.handle});
        if (!Active(handle) || !Seek(handle, state.ambience.playbackFrame))
            return false;
        if (state.ambience.paused && !SetAmbiencePaused(true)) return false;
    }
    return true;
}

bool AudioEngine::RestoreMusicTrack(const TrackState& state, const int volume) {
    if (volume < 0 || volume > 128 || state.playbackFrame < 0) return false;
    SetBGMVolume(volume);
    StopBGM(0);
    if (!state.playing || state.path.empty()) return true;
    if (!m_initialized) return false;
    const AudioHandle handle = PlayWithHandle(
        {.bus=Bus::Music,.path=state.path,.loop=state.loop},
        AudioHandle{state.handle});
    if (!Active(handle) || !Seek(handle, state.playbackFrame)) return false;
    return !state.paused || SetMusicPaused(true);
}

bool AudioEngine::RestoreVoiceTrack(const TrackState& state, const int volume) {
    if (volume < 0 || volume > 128 || state.playbackFrame < 0) return false;
    SetVoiceVolume(volume);
    StopVoice();
    if (!state.playing || state.path.empty()) return true;
    if (!m_initialized) return false;
    const AudioHandle handle = PlayWithHandle(
        {.bus=Bus::Voice,.path=state.path}, AudioHandle{state.handle});
    if (!Active(handle) || !Seek(handle, state.playbackFrame)) return false;
    return !state.paused || SetVoicePaused(true);
}

bool AudioEngine::RestoreAmbienceTrack(const TrackState& state,
                                       const int volume) {
    if (volume < 0 || volume > 128 || state.playbackFrame < 0) return false;
    SetAmbienceVolume(volume);
    StopAmbience(0);
    if (!state.playing || state.path.empty()) return true;
    if (!m_initialized) return false;
    const AudioHandle handle = PlayWithHandle(
        {.bus=Bus::Ambience,.path=state.path,.loop=state.loop},
        AudioHandle{state.handle});
    if (!Active(handle) || !Seek(handle, state.playbackFrame)) return false;
    return !state.paused || SetAmbiencePaused(true);
}

bool AudioEngine::SetTrackPaused(MIX_Track* track, const bool paused) {
    if (!track) return true;
    if (paused) {
        if (MIX_TrackPaused(track) || !MIX_TrackPlaying(track)) return true;
        return MIX_PauseTrack(track);
    }
    return !MIX_TrackPaused(track) || MIX_ResumeTrack(track);
}

bool AudioEngine::SetMusicPaused(const bool paused) {
    if (!m_musicHandle)
        return SetTrackPaused(m_bgmTracks[m_bgmActive], paused);
    return paused ? Pause(m_musicHandle) : Resume(m_musicHandle);
}

bool AudioEngine::SetVoicePaused(const bool paused) {
    if (!m_voiceHandle) return SetTrackPaused(m_voiceTrack, paused);
    return paused ? Pause(m_voiceHandle) : Resume(m_voiceHandle);
}

bool AudioEngine::SetAmbiencePaused(const bool paused) {
    if (!m_ambienceHandle) return SetTrackPaused(m_ambienceTrack, paused);
    return paused ? Pause(m_ambienceHandle) : Resume(m_ambienceHandle);
}

void AudioEngine::SetBGMVolume(int volume) {
    m_bgmVolume = std::clamp(volume,0,128);ApplyMixGains();
}

void AudioEngine::SetSEVolume(int volume) {
    m_seVolume = std::clamp(volume,0,128);ApplyMixGains();
}

void AudioEngine::SetVoiceVolume(int volume) {
    m_voiceVolume = std::clamp(volume,0,128);ApplyMixGains();
}

void AudioEngine::ApplyMixGains(){const float main=ToGain(m_mainVolume),duck=m_voiceDucking?0.45f:1.0f;for(auto* track:m_bgmTracks)if(track)MIX_SetTrackGain(track,ToGain(m_bgmVolume)*main*duck);for(auto* track:m_seTracks)if(track)MIX_SetTrackGain(track,ToGain(m_seVolume)*main);if(m_voiceTrack)MIX_SetTrackGain(m_voiceTrack,ToGain(m_voiceVolume)*main);if(m_ambienceTrack)MIX_SetTrackGain(m_ambienceTrack,ToGain(m_ambienceVolume)*main*duck);}
void AudioEngine::SetAmbienceVolume(int volume){m_ambienceVolume=std::clamp(volume,0,128);ApplyMixGains();}
void AudioEngine::SetMainVolume(int volume){m_mainVolume=std::clamp(volume,0,128);ApplyMixGains();}

}
