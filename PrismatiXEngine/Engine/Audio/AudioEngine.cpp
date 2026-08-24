#include "Engine/Audio/AudioEngine.h"

#include "Engine/Support/Logger.h"

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include <algorithm>

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
    m_mixer = MIX_CreateMixer(&spec);
    if (!m_mixer) {
        PX_LOG_ERROR("MIX_CreateMixer failed: {}", SDL_GetError());
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

void AudioEngine::PlayBGM(const std::string& path, bool loop, int fadeMs) {
    if (!m_initialized) {
        return;
    }
    m_pendingLoop.clear();
    MIX_Track* current = m_bgmTracks[m_bgmActive];
    const bool crossfade = fadeMs > 0 && current && MIX_TrackPlaying(current);
    const int next = crossfade ? 1 - m_bgmActive : m_bgmActive;
    MIX_Track* track = m_bgmTracks[next];
    if (!track) {
        return;
    }

    MIX_StopTrack(track, 0);
    MIX_Audio* audio = AcquireAudio(path, /*predecode=*/false);
    if (!audio || !MIX_SetTrackAudio(track, audio)) {
        return;
    }
    m_bgmAudio[next] = audio;
    ApplyMixGains();

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, MIX_PROP_PLAY_LOOPS_NUMBER, loop ? -1 : 0);
    if (fadeMs > 0) {
        SDL_SetNumberProperty(props, MIX_PROP_PLAY_FADE_IN_MILLISECONDS_NUMBER, fadeMs);
    }
    MIX_PlayTrack(track, props);
    SDL_DestroyProperties(props);
    m_bgmPath = path;
    m_bgmLoop = loop;

    if (crossfade) {
        MIX_StopTrack(current, MIX_MSToFrames(m_sampleRate, fadeMs));
        m_bgmActive = next;
    }
}

void AudioEngine::PlayBGMWithIntro(const std::string& introPath, const std::string& loopPath,
                                   int fadeMs) {
    PlayBGM(introPath, /*loop=*/false, fadeMs);
    m_pendingLoop = loopPath;
}

void AudioEngine::Update() {
    if (!m_initialized) return;
    const bool duck = m_voiceTrack && MIX_TrackPlaying(m_voiceTrack);
    if (duck != m_voiceDucking) {
        m_voiceDucking = duck;
        ApplyMixGains();
    }
    if (m_pendingLoop.empty()) return;
    MIX_Track* track = m_bgmTracks[m_bgmActive];
    if (track && MIX_TrackPlaying(track)) {
        return;
    }
    const std::string loop = m_pendingLoop;
    m_pendingLoop.clear();
    PlayBGM(loop, /*loop=*/true, 0);
}

void AudioEngine::PlayAmbience(const std::string& path,bool loop,int fadeMs){if(!m_initialized||!m_ambienceTrack)return;MIX_Audio* audio=AcquireAudio(path,false);if(!audio||!MIX_SetTrackAudio(m_ambienceTrack,audio))return;m_ambienceAudio=audio;SDL_PropertiesID props=SDL_CreateProperties();SDL_SetNumberProperty(props,MIX_PROP_PLAY_LOOPS_NUMBER,loop?-1:0);if(fadeMs>0)SDL_SetNumberProperty(props,MIX_PROP_PLAY_FADE_IN_MILLISECONDS_NUMBER,fadeMs);ApplyMixGains();MIX_PlayTrack(m_ambienceTrack,props);SDL_DestroyProperties(props);m_ambiencePath=path;m_ambienceLoop=loop;}
void AudioEngine::StopAmbience(int fadeMs){if(m_initialized&&m_ambienceTrack)MIX_StopTrack(m_ambienceTrack,fadeMs>0?MIX_MSToFrames(m_sampleRate,fadeMs):0);m_ambiencePath.clear();}

void AudioEngine::StopBGM(int fadeMs) {
    if (!m_initialized) {
        return;
    }
    m_pendingLoop.clear();
    const Sint64 frames = fadeMs > 0 ? MIX_MSToFrames(m_sampleRate, fadeMs) : 0;
    for (MIX_Track* track : m_bgmTracks) {
        if (track) {
            MIX_StopTrack(track, frames);
        }
    }
    m_bgmPath.clear();
}

void AudioEngine::PlaySE(const std::string& path) {
    if (!m_initialized) {
        return;
    }
    MIX_Audio* audio = AcquireAudio(path, /*predecode=*/true);
    if (!audio) {
        return;
    }
    MIX_Track* track = AcquireSeTrack(audio);
    if (!track) {
        return;
    }
    MIX_StopTrack(track, 0);
    MIX_SetTrackAudio(track, audio);
    ApplyMixGains();
    MIX_PlayTrack(track, 0);
}

void AudioEngine::PlayVoice(const std::string& path) {
    if (!m_initialized) {
        return;
    }
    // Voice lines are unique per dialogue line — caching them would grow without
    // bound over a session, so each voice replaces the previous one.
    auto bytes = m_vfs.Read(path);
    if (!bytes) {
        PX_LOG_WARN("AudioEngine: audio not found '{}'", path);
        return;
    }
    MIX_StopTrack(m_voiceTrack, 0);
    if (m_voiceAudio.audio) {
        MIX_SetTrackAudio(m_voiceTrack, nullptr);
        MIX_DestroyAudio(m_voiceAudio.audio);
        m_voiceAudio = {};
    }
    auto held = std::make_shared<io::Bytes>(std::move(*bytes));
    SDL_IOStream* io = SDL_IOFromConstMem(held->data(), held->size());
    MIX_Audio* audio = io ? MIX_LoadAudio_IO(m_mixer, io, /*predecode=*/true, /*closeio=*/true)
                          : nullptr;
    if (!audio) {
        PX_LOG_WARN("AudioEngine: failed to load voice '{}': {}", path, SDL_GetError());
        return;
    }
    m_voiceAudio = AudioEntry{ audio, held, 0 };
    MIX_SetTrackAudio(m_voiceTrack, audio);
    ApplyMixGains();
    MIX_PlayTrack(m_voiceTrack, 0);
    m_voicePath = path;
}

void AudioEngine::StopVoice() {
    if (m_initialized) {
        MIX_StopTrack(m_voiceTrack, 0);
    }
    m_voicePath.clear();
}

AudioEngine::RuntimeState AudioEngine::CaptureState() const {
    const auto trackState = [](MIX_Track* track, const std::string& path, const bool loop) {
        TrackState state{.path=path,.loop=loop};
        if (track && !path.empty()) {
            state.playing = MIX_TrackPlaying(track);
            state.playbackFrame = std::max<Sint64>(0, MIX_GetTrackPlaybackPosition(track));
        }
        return state;
    };
    RuntimeState state;
    state.music = trackState(m_bgmTracks[m_bgmActive], m_bgmPath, m_bgmLoop);
    state.voice = trackState(m_voiceTrack, m_voicePath, false);
    state.ambience = trackState(m_ambienceTrack, m_ambiencePath, m_ambienceLoop);
    state.pendingMusicLoop = m_pendingLoop;
    state.mainVolume = m_mainVolume;
    state.musicVolume = m_bgmVolume;
    state.voiceVolume = m_voiceVolume;
    state.sfxVolume = m_seVolume;
    state.ambienceVolume = m_ambienceVolume;
    return state;
}

bool AudioEngine::RestoreState(const RuntimeState& state) {
    if (state.mainVolume < 0 || state.mainVolume > 128 || state.musicVolume < 0 ||
        state.musicVolume > 128 || state.voiceVolume < 0 || state.voiceVolume > 128 ||
        state.sfxVolume < 0 || state.sfxVolume > 128 || state.ambienceVolume < 0 ||
        state.ambienceVolume > 128 || state.music.playbackFrame < 0 ||
        state.voice.playbackFrame < 0 || state.ambience.playbackFrame < 0) return false;
    SetMainVolume(state.mainVolume);
    SetBGMVolume(state.musicVolume);
    SetVoiceVolume(state.voiceVolume);
    SetSEVolume(state.sfxVolume);
    SetAmbienceVolume(state.ambienceVolume);
    StopBGM(0); StopVoice(); StopAmbience(0);
    if (state.music.playing && !state.music.path.empty()) {
        PlayBGM(state.music.path, state.music.loop, 0);
        if (!MIX_SetTrackPlaybackPosition(m_bgmTracks[m_bgmActive], state.music.playbackFrame))
            return false;
    }
    if (state.voice.playing && !state.voice.path.empty()) {
        PlayVoice(state.voice.path);
        if (!MIX_SetTrackPlaybackPosition(m_voiceTrack, state.voice.playbackFrame)) return false;
    }
    if (state.ambience.playing && !state.ambience.path.empty()) {
        PlayAmbience(state.ambience.path, state.ambience.loop, 0);
        if (!MIX_SetTrackPlaybackPosition(m_ambienceTrack, state.ambience.playbackFrame)) return false;
    }
    m_pendingLoop = state.pendingMusicLoop;
    return true;
}

bool AudioEngine::RestoreMusicTrack(const TrackState& state, const int volume) {
    if (volume < 0 || volume > 128 || state.playbackFrame < 0) return false;
    SetBGMVolume(volume);
    StopBGM(0);
    if (!state.playing || state.path.empty()) return true;
    PlayBGM(state.path, state.loop, 0);
    return MIX_SetTrackPlaybackPosition(m_bgmTracks[m_bgmActive],
                                        state.playbackFrame);
}

bool AudioEngine::RestoreVoiceTrack(const TrackState& state, const int volume) {
    if (volume < 0 || volume > 128 || state.playbackFrame < 0) return false;
    SetVoiceVolume(volume);
    StopVoice();
    if (!state.playing || state.path.empty()) return true;
    PlayVoice(state.path);
    return MIX_SetTrackPlaybackPosition(m_voiceTrack, state.playbackFrame);
}

bool AudioEngine::RestoreAmbienceTrack(const TrackState& state,
                                       const int volume) {
    if (volume < 0 || volume > 128 || state.playbackFrame < 0) return false;
    SetAmbienceVolume(volume);
    StopAmbience(0);
    if (!state.playing || state.path.empty()) return true;
    PlayAmbience(state.path, state.loop, 0);
    return MIX_SetTrackPlaybackPosition(m_ambienceTrack,
                                        state.playbackFrame);
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
    return SetTrackPaused(m_bgmTracks[m_bgmActive], paused);
}

bool AudioEngine::SetVoicePaused(const bool paused) {
    return SetTrackPaused(m_voiceTrack, paused);
}

bool AudioEngine::SetAmbiencePaused(const bool paused) {
    return SetTrackPaused(m_ambienceTrack, paused);
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
