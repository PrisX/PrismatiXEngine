#include "Engine/Audio/AudioEngine.h"

#include "Engine/Support/Logger.h"

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

namespace px::audio {

namespace {
constexpr std::size_t kMaxSeTracks = 12;

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
    m_bgmTrack = MIX_CreateTrack(m_mixer);
    m_voiceTrack = MIX_CreateTrack(m_mixer);
    m_initialized = true;
    PX_LOG_INFO("AudioEngine initialized ({} Hz).", m_sampleRate);
    return true;
}

void AudioEngine::Shutdown() {
    if (!m_initialized) {
        return;
    }
    if (m_bgmTrack) MIX_DestroyTrack(m_bgmTrack), m_bgmTrack = nullptr;
    if (m_voiceTrack) MIX_DestroyTrack(m_voiceTrack), m_voiceTrack = nullptr;
    for (MIX_Track* t : m_seTracks) {
        MIX_DestroyTrack(t);
    }
    m_seTracks.clear();
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
    if (auto it = m_audio.find(path); it != m_audio.end()) {
        return it->second.audio;
    }
    auto bytes = m_vfs.Read(path);
    if (!bytes) {
        PX_LOG_WARN("AudioEngine: audio not found '{}'", path);
        m_audio[path] = {};
        return nullptr;
    }
    auto held = std::make_shared<io::Bytes>(std::move(*bytes));
    SDL_IOStream* io = SDL_IOFromConstMem(held->data(), held->size());
    MIX_Audio* audio = io ? MIX_LoadAudio_IO(m_mixer, io, predecode, /*closeio=*/true) : nullptr;
    if (!audio) {
        PX_LOG_WARN("AudioEngine: failed to load audio '{}': {}", path, SDL_GetError());
        m_audio[path] = {};
        return nullptr;
    }
    m_audio[path] = AudioEntry{ audio, held };
    return audio;
}

MIX_Track* AudioEngine::AcquireSeTrack() {
    for (MIX_Track* t : m_seTracks) {
        if (!MIX_TrackPlaying(t)) {
            return t;
        }
    }
    if (m_seTracks.size() < kMaxSeTracks) {
        MIX_Track* t = MIX_CreateTrack(m_mixer);
        if (t) {
            m_seTracks.push_back(t);
        }
        return t;
    }
    return m_seTracks.front();
}

void AudioEngine::PlayBGM(const std::string& path, bool loop, int fadeMs) {
    if (!m_initialized) {
        return;
    }
    MIX_StopTrack(m_bgmTrack, 0);
    MIX_Audio* audio = AcquireAudio(path, /*predecode=*/false);
    if (!audio || !MIX_SetTrackAudio(m_bgmTrack, audio)) {
        return;
    }
    MIX_SetTrackGain(m_bgmTrack, ToGain(m_bgmVolume));

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, MIX_PROP_PLAY_LOOPS_NUMBER, loop ? -1 : 0);
    if (fadeMs > 0) {
        SDL_SetNumberProperty(props, MIX_PROP_PLAY_FADE_IN_MILLISECONDS_NUMBER, fadeMs);
    }
    MIX_PlayTrack(m_bgmTrack, props);
    SDL_DestroyProperties(props);
}

void AudioEngine::StopBGM(int fadeMs) {
    if (!m_initialized) {
        return;
    }
    const Sint64 frames = fadeMs > 0 ? MIX_MSToFrames(m_sampleRate, fadeMs) : 0;
    MIX_StopTrack(m_bgmTrack, frames);
}

void AudioEngine::PlaySE(const std::string& path) {
    if (!m_initialized) {
        return;
    }
    MIX_Audio* audio = AcquireAudio(path, /*predecode=*/true);
    MIX_Track* track = AcquireSeTrack();
    if (!audio || !track) {
        return;
    }
    MIX_StopTrack(track, 0);
    MIX_SetTrackAudio(track, audio);
    MIX_SetTrackGain(track, ToGain(m_seVolume));
    MIX_PlayTrack(track, 0);
}

void AudioEngine::PlayVoice(const std::string& path) {
    if (!m_initialized) {
        return;
    }
    MIX_Audio* audio = AcquireAudio(path, /*predecode=*/true);
    if (!audio) {
        return;
    }
    MIX_StopTrack(m_voiceTrack, 0);
    MIX_SetTrackAudio(m_voiceTrack, audio);
    MIX_SetTrackGain(m_voiceTrack, ToGain(m_voiceVolume));
    MIX_PlayTrack(m_voiceTrack, 0);
}

void AudioEngine::StopVoice() {
    if (m_initialized) {
        MIX_StopTrack(m_voiceTrack, 0);
    }
}

void AudioEngine::SetBGMVolume(int volume) {
    m_bgmVolume = volume;
    if (m_initialized) {
        MIX_SetTrackGain(m_bgmTrack, ToGain(volume));
    }
}

void AudioEngine::SetSEVolume(int volume) {
    m_seVolume = volume;
}

void AudioEngine::SetVoiceVolume(int volume) {
    m_voiceVolume = volume;
}

}
