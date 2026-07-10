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
        if (audio && (audio == m_bgmAudio[0] || audio == m_bgmAudio[1])) {
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
    MIX_SetTrackGain(track, ToGain(m_bgmVolume));

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, MIX_PROP_PLAY_LOOPS_NUMBER, loop ? -1 : 0);
    if (fadeMs > 0) {
        SDL_SetNumberProperty(props, MIX_PROP_PLAY_FADE_IN_MILLISECONDS_NUMBER, fadeMs);
    }
    MIX_PlayTrack(track, props);
    SDL_DestroyProperties(props);

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
    if (!m_initialized || m_pendingLoop.empty()) {
        return;
    }
    MIX_Track* track = m_bgmTracks[m_bgmActive];
    if (track && MIX_TrackPlaying(track)) {
        return;
    }
    const std::string loop = m_pendingLoop;
    m_pendingLoop.clear();
    PlayBGM(loop, /*loop=*/true, 0);
}

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
    MIX_SetTrackGain(track, ToGain(m_seVolume));
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
        for (MIX_Track* track : m_bgmTracks) {
            if (track) {
                MIX_SetTrackGain(track, ToGain(volume));
            }
        }
    }
}

void AudioEngine::SetSEVolume(int volume) {
    m_seVolume = volume;
    if (m_initialized) {
        for (MIX_Track* t : m_seTracks) {
            MIX_SetTrackGain(t, ToGain(volume));
        }
    }
}

void AudioEngine::SetVoiceVolume(int volume) {
    m_voiceVolume = volume;
    if (m_initialized) {
        MIX_SetTrackGain(m_voiceTrack, ToGain(volume));
    }
}

}
