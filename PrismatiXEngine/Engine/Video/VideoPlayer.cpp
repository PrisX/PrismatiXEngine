#include "Engine/Video/VideoPlayer.h"

#include "Engine/Support/Logger.h"

#include <SDL3/SDL.h>

#define PL_MPEG_IMPLEMENTATION
#include <pl_mpeg.h>

#include <algorithm>

namespace px::video {

namespace {
void OnVideoCb(plm_t*, plm_frame_t* frame, void* user) {
    static_cast<VideoPlayer*>(user)->HandleVideoFrame(frame);
}
void OnAudioCb(plm_t*, plm_samples_t* samples, void* user) {
    static_cast<VideoPlayer*>(user)->HandleAudioSamples(samples);
}
}

VideoPlayer::VideoPlayer(SDL_Renderer* renderer, io::VFS& vfs)
    : m_renderer(renderer), m_vfs(vfs) {}

VideoPlayer::~VideoPlayer() {
    Close();
}

bool VideoPlayer::Open(const std::string& vfsPath, float volume) {
    Close();

    auto bytes = m_vfs.Read(vfsPath);
    if (!bytes) {
        PX_LOG_WARN("VideoPlayer: video not found '{}'", vfsPath);
        return false;
    }
    m_data = std::move(*bytes);
    m_plm = plm_create_with_memory(m_data.data(), m_data.size(), /*free_when_done=*/0);
    if (!m_plm || !plm_has_headers(m_plm)) {
        PX_LOG_WARN("VideoPlayer: not a valid MPEG-PS/MPEG-1 file '{}'", vfsPath);
        Close();
        return false;
    }

    m_width = plm_get_width(m_plm);
    m_height = plm_get_height(m_plm);
    if (m_width <= 0 || m_height <= 0) {
        PX_LOG_WARN("VideoPlayer: no video stream in '{}'", vfsPath);
        Close();
        return false;
    }
    m_texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGB24,
                                  SDL_TEXTUREACCESS_STREAMING, m_width, m_height);
    if (!m_texture) {
        PX_LOG_ERROR("VideoPlayer: texture creation failed: {}", SDL_GetError());
        Close();
        return false;
    }
    SDL_SetTextureScaleMode(m_texture, SDL_SCALEMODE_LINEAR);
    m_rgb = std::make_unique<std::uint8_t[]>(static_cast<std::size_t>(m_width) * m_height * 3);

    m_volume = std::clamp(volume, 0.0f, 1.0f);
    plm_set_video_decode_callback(m_plm, &OnVideoCb, this);
    if (plm_get_num_audio_streams(m_plm) > 0) {
        SDL_AudioSpec spec{};
        spec.format = SDL_AUDIO_F32;
        spec.channels = 2;
        spec.freq = plm_get_samplerate(m_plm);
        m_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr,
                                            nullptr);
        if (m_audio) {
            SDL_ResumeAudioStreamDevice(m_audio);
            plm_set_audio_decode_callback(m_plm, &OnAudioCb, this);
            plm_set_audio_enabled(m_plm, 1);
            plm_set_audio_stream(m_plm, 0);
            // Keep a little audio queued ahead of the video clock.
            plm_set_audio_lead_time(m_plm, 0.05);
        } else {
            PX_LOG_WARN("VideoPlayer: audio device unavailable: {}", SDL_GetError());
            plm_set_audio_enabled(m_plm, 0);
        }
    } else {
        plm_set_audio_enabled(m_plm, 0);
    }
    plm_set_loop(m_plm, 0);
    m_finished = false;

    PX_LOG_INFO("VideoPlayer: playing '{}' ({}x{}, {:.1f}s)", vfsPath, m_width, m_height,
                plm_get_duration(m_plm));
    return true;
}

void VideoPlayer::Close() {
    if (m_audio) {
        SDL_DestroyAudioStream(m_audio);
        m_audio = nullptr;
    }
    if (m_texture) {
        SDL_DestroyTexture(m_texture);
        m_texture = nullptr;
    }
    if (m_plm) {
        plm_destroy(m_plm);
        m_plm = nullptr;
    }
    m_rgb.reset();
    m_data.clear();
    m_data.shrink_to_fit();
    m_width = m_height = 0;
    m_finished = false;
}

void VideoPlayer::HandleVideoFrame(void* framePtr) {
    auto* frame = static_cast<plm_frame_t*>(framePtr);
    plm_frame_to_rgb(frame, m_rgb.get(), m_width * 3);
    SDL_UpdateTexture(m_texture, nullptr, m_rgb.get(), m_width * 3);
}

void VideoPlayer::HandleAudioSamples(void* samplesPtr) {
    auto* samples = static_cast<plm_samples_t*>(samplesPtr);
    if (!m_audio) {
        return;
    }
    if (m_volume < 0.999f) {
        for (unsigned int i = 0; i < samples->count * 2; ++i) {
            samples->interleaved[i] *= m_volume;
        }
    }
    SDL_PutAudioStreamData(m_audio, samples->interleaved,
                           static_cast<int>(samples->count * 2 * sizeof(float)));
}

void VideoPlayer::Update(float dt) {
    if (!m_plm || m_finished) {
        return;
    }
    // Clamp huge frame spikes (window drags etc.) so we never decode seconds at once.
    plm_decode(m_plm, std::min(dt, 0.25f));
    if (plm_has_ended(m_plm)) {
        m_finished = true;
    }
}

void VideoPlayer::Render(int logicalW, int logicalH) {
    if (!m_texture || m_width <= 0 || m_height <= 0) {
        return;
    }
    SDL_FRect dst;
    const float scale = std::min(static_cast<float>(logicalW) / m_width,
                                 static_cast<float>(logicalH) / m_height);
    dst.w = m_width * scale;
    dst.h = m_height * scale;
    dst.x = (logicalW - dst.w) * 0.5f;
    dst.y = (logicalH - dst.h) * 0.5f;
    SDL_RenderTexture(m_renderer, m_texture, nullptr, &dst);
}

}
