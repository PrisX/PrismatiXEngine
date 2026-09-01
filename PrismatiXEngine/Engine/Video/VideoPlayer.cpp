#include "Engine/Video/VideoPlayer.h"

#include "Engine/Support/Logger.h"

#include <SDL3/SDL.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244 4267 4305)
#endif
#define PL_MPEG_IMPLEMENTATION
#include <pl_mpeg.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace px::video {

struct FfmpegState {
    AVFormatContext* format = nullptr;
    AVCodecContext* codec = nullptr;
    AVCodecContext* audioCodec = nullptr;
    AVIOContext* io = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* audioFrame = nullptr;
    SwsContext* scaler = nullptr;
    SwrContext* resampler = nullptr;
    int stream = -1;
    int audioStream = -1;
    std::size_t position = 0;
    double clock = 0.0;
    double pendingTime = 0.0;
    bool pending = false;
    bool eofSent = false;
    bool decodeEnded = false;
};

namespace {
void OnVideoCb(plm_t*, plm_frame_t* frame, void* user) {
    static_cast<VideoPlayer*>(user)->HandleVideoFrame(frame);
}
void OnAudioCb(plm_t*, plm_samples_t* samples, void* user) {
    static_cast<VideoPlayer*>(user)->HandleAudioSamples(samples);
}
}

int VideoPlayer::ReadFfmpeg(void* opaque, std::uint8_t* buffer, const int size) {
    auto* player = static_cast<VideoPlayer*>(opaque);
    auto& state = *player->m_ffmpeg;
    if (state.position >= player->m_data.size()) return AVERROR_EOF;
    const auto count = std::min<std::size_t>(static_cast<std::size_t>(size),
                                             player->m_data.size() - state.position);
    std::memcpy(buffer, player->m_data.data() + state.position, count);
    state.position += count;
    return static_cast<int>(count);
}

std::int64_t VideoPlayer::SeekFfmpeg(void* opaque, const std::int64_t offset, const int whence) {
    auto* player = static_cast<VideoPlayer*>(opaque);
    auto& state = *player->m_ffmpeg;
    if (whence == AVSEEK_SIZE) return static_cast<std::int64_t>(player->m_data.size());
    const int origin = whence & ~AVSEEK_FORCE;
    std::int64_t position = 0;
    if (origin == SEEK_SET) position = offset;
    else if (origin == SEEK_CUR) position = static_cast<std::int64_t>(state.position) + offset;
    else if (origin == SEEK_END) position = static_cast<std::int64_t>(player->m_data.size()) + offset;
    else return AVERROR(EINVAL);
    if (position < 0 || static_cast<std::uint64_t>(position) > player->m_data.size())
        return AVERROR(EINVAL);
    state.position = static_cast<std::size_t>(position);
    return position;
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
    std::string extension = std::filesystem::path(vfsPath).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
    m_volume = std::clamp(volume, 0.0f, 1.0f);
    if (extension != ".mpg" && extension != ".mpeg") {
        if (OpenFfmpeg(vfsPath)) return true;
        Close();
        return false;
    }
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
    m_texture = graphics::TextureResource::CreateStreaming(
        graphics::TextureBackend::SdlRenderer, m_renderer,
        graphics::StreamingTextureFormat::Rgb24, m_width, m_height);
    if (!m_texture) {
        PX_LOG_ERROR("VideoPlayer: texture creation failed: {}", SDL_GetError());
        Close();
        return false;
    }
    (void)m_texture.SetLinearSampling();
    m_rgb = std::make_unique<std::uint8_t[]>(static_cast<std::size_t>(m_width) * m_height * 3);

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

bool VideoPlayer::OpenFfmpeg(const std::string& vfsPath) {
    m_ffmpeg = std::make_unique<FfmpegState>();
    auto& state = *m_ffmpeg;
    state.format = avformat_alloc_context();
    auto* ioBuffer = static_cast<unsigned char*>(av_malloc(64 * 1024));
    if (!state.format || !ioBuffer) {
        PX_LOG_ERROR("VideoPlayer: FFmpeg allocation failed");
        CloseFfmpeg();
        return false;
    }
    state.io = avio_alloc_context(ioBuffer, 64 * 1024, 0, this, &VideoPlayer::ReadFfmpeg,
                                  nullptr, &VideoPlayer::SeekFfmpeg);
    if (!state.io) {
        av_free(ioBuffer);
        CloseFfmpeg();
        return false;
    }
    state.format->pb = state.io;
    state.format->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&state.format, nullptr, nullptr, nullptr) < 0 ||
        avformat_find_stream_info(state.format, nullptr) < 0) {
        PX_LOG_WARN("VideoPlayer: FFmpeg cannot inspect '{}'", vfsPath);
        CloseFfmpeg();
        return false;
    }
    const AVCodec* decoder = nullptr;
    state.stream = av_find_best_stream(state.format, AVMEDIA_TYPE_VIDEO, -1, -1,
                                       &decoder, 0);
    if (state.stream < 0 || !decoder) {
        PX_LOG_WARN("VideoPlayer: no supported video stream in '{}'", vfsPath);
        CloseFfmpeg();
        return false;
    }
    state.codec = avcodec_alloc_context3(decoder);
    const AVCodecParameters* parameters = state.format->streams[state.stream]->codecpar;
    if (!state.codec || avcodec_parameters_to_context(state.codec, parameters) < 0 ||
        avcodec_open2(state.codec, decoder, nullptr) < 0) {
        PX_LOG_WARN("VideoPlayer: cannot open codec for '{}'", vfsPath);
        CloseFfmpeg();
        return false;
    }
    const AVCodec* audioDecoder = nullptr;
    state.audioStream = av_find_best_stream(state.format, AVMEDIA_TYPE_AUDIO, -1,
                                            state.stream, &audioDecoder, 0);
    if (state.audioStream >= 0 && audioDecoder) {
        state.audioCodec = avcodec_alloc_context3(audioDecoder);
        const auto* audioParameters = state.format->streams[state.audioStream]->codecpar;
        if (!state.audioCodec ||
            avcodec_parameters_to_context(state.audioCodec, audioParameters) < 0 ||
            avcodec_open2(state.audioCodec, audioDecoder, nullptr) < 0) {
            avcodec_free_context(&state.audioCodec);
            state.audioStream = -1;
        } else {
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&state.resampler, &stereo, AV_SAMPLE_FMT_FLT, 48000,
                                    &state.audioCodec->ch_layout, state.audioCodec->sample_fmt,
                                    state.audioCodec->sample_rate, 0, nullptr) < 0 ||
                swr_init(state.resampler) < 0) {
                swr_free(&state.resampler);
                avcodec_free_context(&state.audioCodec);
                state.audioStream = -1;
            } else {
                state.audioFrame = av_frame_alloc();
                SDL_AudioSpec spec{}; spec.format = SDL_AUDIO_F32; spec.channels = 2; spec.freq = 48000;
                m_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                                    nullptr, nullptr);
                if (m_audio) SDL_ResumeAudioStreamDevice(m_audio);
                else PX_LOG_WARN("VideoPlayer: FFmpeg audio device unavailable: {}", SDL_GetError());
            }
        }
    }
    m_width = state.codec->width;
    m_height = state.codec->height;
    if (m_width <= 0 || m_height <= 0 || m_width > 16384 || m_height > 16384) {
        PX_LOG_WARN("VideoPlayer: invalid FFmpeg dimensions in '{}'", vfsPath);
        CloseFfmpeg();
        return false;
    }
    state.packet = av_packet_alloc();
    state.frame = av_frame_alloc();
    state.scaler = sws_getContext(m_width, m_height, state.codec->pix_fmt, m_width, m_height,
                                  AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
    m_texture = graphics::TextureResource::CreateStreaming(
        graphics::TextureBackend::SdlRenderer, m_renderer,
        graphics::StreamingTextureFormat::Rgb24, m_width, m_height);
    m_rgb = std::make_unique<std::uint8_t[]>(static_cast<std::size_t>(m_width) * m_height * 3);
    if (!state.packet || !state.frame || !state.scaler || !m_texture || !m_rgb) {
        PX_LOG_ERROR("VideoPlayer: FFmpeg frame resources could not be created");
        CloseFfmpeg();
        return false;
    }
    (void)m_texture.SetLinearSampling();
    m_finished = false;
    if (!DecodeNextFfmpegFrame()) {
        PX_LOG_WARN("VideoPlayer: '{}' contains no decodable video frames", vfsPath);
        CloseFfmpeg();
        return false;
    }
    UpdateFfmpeg(0.0f);
    PX_LOG_INFO("VideoPlayer: FFmpeg playing '{}' ({}x{})", vfsPath, m_width, m_height);
    return true;
}

bool VideoPlayer::DecodeNextFfmpegFrame() {
    if (!m_ffmpeg) return false;
    auto& state = *m_ffmpeg;
    for (;;) {
        const int received = avcodec_receive_frame(state.codec, state.frame);
        if (received == 0) {
            std::uint8_t* destination[] = {m_rgb.get(), nullptr, nullptr, nullptr};
            const int strides[] = {m_width * 3, 0, 0, 0};
            sws_scale(state.scaler, state.frame->data, state.frame->linesize, 0, m_height,
                      destination, strides);
            const auto timestamp = state.frame->best_effort_timestamp;
            state.pendingTime = timestamp == AV_NOPTS_VALUE ? state.clock
                : timestamp * av_q2d(state.format->streams[state.stream]->time_base);
            state.pending = true;
            av_frame_unref(state.frame);
            return true;
        }
        if (received == AVERROR_EOF) {
            state.decodeEnded = true;
            return false;
        }
        if (received != AVERROR(EAGAIN)) {
            state.decodeEnded = true;
            return false;
        }
        bool submitted = false;
        while (!submitted) {
            const int read = av_read_frame(state.format, state.packet);
            if (read < 0) {
                if (!state.eofSent) {
                    avcodec_send_packet(state.codec, nullptr);
                    if (state.audioCodec) {
                        avcodec_send_packet(state.audioCodec, nullptr);
                        DecodeFfmpegAudio(nullptr);
                    }
                    state.eofSent = true;
                    submitted = true;
                    break;
                }
                state.decodeEnded = true;
                return false;
            }
            if (state.packet->stream_index == state.stream) {
                const int sent = avcodec_send_packet(state.codec, state.packet);
                submitted = sent >= 0 || sent == AVERROR(EAGAIN);
            } else if (state.packet->stream_index == state.audioStream) {
                DecodeFfmpegAudio(state.packet);
            }
            av_packet_unref(state.packet);
        }
    }
}

void VideoPlayer::DecodeFfmpegAudio(void* packetValue) {
    if (!m_ffmpeg || !m_ffmpeg->audioCodec || !m_ffmpeg->audioFrame ||
        !m_ffmpeg->resampler) return;
    auto& state = *m_ffmpeg;
    auto* packet = static_cast<AVPacket*>(packetValue);
    if (packet && avcodec_send_packet(state.audioCodec, packet) < 0) return;
    while (avcodec_receive_frame(state.audioCodec, state.audioFrame) == 0) {
        const int capacity = swr_get_out_samples(state.resampler, state.audioFrame->nb_samples);
        if (capacity <= 0 || capacity > 48000 * 8) { av_frame_unref(state.audioFrame); continue; }
        std::vector<float> samples(static_cast<std::size_t>(capacity) * 2);
        std::uint8_t* output[] = {reinterpret_cast<std::uint8_t*>(samples.data())};
        const int converted = swr_convert(state.resampler, output, capacity,
            const_cast<const std::uint8_t**>(state.audioFrame->extended_data),
            state.audioFrame->nb_samples);
        if (converted > 0 && m_audio) {
            samples.resize(static_cast<std::size_t>(converted) * 2);
            if (m_volume < 0.999f) for (float& sample : samples) sample *= m_volume;
            SDL_PutAudioStreamData(m_audio, samples.data(),
                static_cast<int>(samples.size() * sizeof(float)));
        }
        av_frame_unref(state.audioFrame);
    }
}

void VideoPlayer::UpdateFfmpeg(const float dt) {
    if (!m_ffmpeg || m_finished) return;
    auto& state = *m_ffmpeg;
    state.clock += std::clamp(static_cast<double>(dt), 0.0, 0.25);
    int decoded = 0;
    while (state.pending && state.pendingTime <= state.clock + 0.001 && decoded < 8) {
        (void)m_texture.Update(m_rgb.get(),
                               static_cast<std::size_t>(m_width) * 3u);
        state.pending = false;
        ++decoded;
        if (!DecodeNextFfmpegFrame()) break;
    }
    if (!state.pending && state.decodeEnded) m_finished = true;
}

void VideoPlayer::CloseFfmpeg() {
    if (!m_ffmpeg) return;
    auto& state = *m_ffmpeg;
    if (state.scaler) sws_freeContext(state.scaler);
    if (state.resampler) swr_free(&state.resampler);
    if (state.audioFrame) av_frame_free(&state.audioFrame);
    if (state.frame) av_frame_free(&state.frame);
    if (state.packet) av_packet_free(&state.packet);
    if (state.codec) avcodec_free_context(&state.codec);
    if (state.audioCodec) avcodec_free_context(&state.audioCodec);
    if (state.format) avformat_close_input(&state.format);
    if (state.io) {
        av_freep(&state.io->buffer);
        avio_context_free(&state.io);
    }
    m_ffmpeg.reset();
}

void VideoPlayer::Close() {
    CloseFfmpeg();
    if (m_audio) {
        SDL_DestroyAudioStream(m_audio);
        m_audio = nullptr;
    }
    m_texture.Reset();
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
    (void)m_texture.Update(m_rgb.get(),
                           static_cast<std::size_t>(m_width) * 3u);
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
    if (m_ffmpeg) {
        UpdateFfmpeg(dt);
        return;
    }
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
    SDL_RenderTexture(
        m_renderer,
        static_cast<SDL_Texture*>(m_texture.Native(
            graphics::TextureBackend::SdlRenderer)),
        nullptr, &dst);
}

}
