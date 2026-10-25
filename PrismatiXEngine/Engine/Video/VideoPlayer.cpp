#include "Engine/Video/VideoPlayer.h"

#include "Engine/Support/Logger.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace px::video {

namespace {
constexpr std::size_t kVideoQueueCapacity = 8;
constexpr std::size_t kAudioQueueCapacity = 32;
constexpr std::size_t kVideoQueueByteCapacity = 256u * 1024u * 1024u;
constexpr std::size_t kAudioQueueByteCapacity = 16u * 1024u * 1024u;
constexpr int kOutputSampleRate = 48'000;
constexpr int kOutputChannels = 2;
constexpr double kAudioQueueLeadSeconds = 0.75;
constexpr double kVideoEarlyToleranceSeconds = 0.015;
constexpr std::size_t kStartupVideoFrames = 3;
constexpr double kStartupAudioSeconds = 0.1;
constexpr std::uint64_t kMaximumStartupWaitNs = 1'000'000'000;
constexpr int kCorruptPacketLimit = 32;
// Compressed packets held back while their stream's decoded queue is full,
// so the demuxer can reach the other stream when a file stores it well
// behind (video waiting for audio, or audio waiting for video).
constexpr std::size_t kPendingVideoPacketCapacity = 1024;
constexpr std::size_t kPendingVideoByteCapacity = 64u * 1024u * 1024u;
constexpr std::size_t kPendingAudioPacketCapacity = 1024;
constexpr std::size_t kPendingAudioByteCapacity = 16u * 1024u * 1024u;

struct DecodedVideoFrame {
    double pts = 0.0;
    std::vector<std::uint8_t> rgb;
};

struct DecodedAudioFrame {
    double pts = 0.0;
    double duration = 0.0;
    std::vector<float> samples;
};

constexpr std::uint64_t kNsPerSecond = 1'000'000'000;

std::string AvError(const int value) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(value, buffer.data(), buffer.size()) < 0)
        return "FFmpeg error " + std::to_string(value);
    return buffer.data();
}

double StreamStartSeconds(const AVFormatContext* format) {
    return format && format->start_time != AV_NOPTS_VALUE
               ? static_cast<double>(format->start_time) /
                     static_cast<double>(AV_TIME_BASE)
               : 0.0;
}

}  // namespace

struct FfmpegState {
    AVFormatContext* format = nullptr;
    AVCodecContext* videoCodec = nullptr;
    AVCodecContext* audioCodec = nullptr;
    AVIOContext* io = nullptr;
    AVFrame* videoFrame = nullptr;
    AVFrame* audioFrame = nullptr;
    SwsContext* scaler = nullptr;
    SwrContext* resampler = nullptr;
    std::unique_ptr<io::SeekableReadStream> stream;
    int videoStream = -1;
    int audioStream = -1;
    int width = 0;
    int height = 0;
    double timeOrigin = 0.0;
    double nextVideoPts = 0.0;
    // Audio time runs from the first audio packet's time by sample count.
    double audioStartPts = -1.0;
    std::uint64_t audioSamplesDecoded = 0;
    double videoFrameDuration = 1.0 / 30.0;
    double audioSubmittedEndPts = 0.0;
    bool audioStarted = false;
    bool outputCreated = false;
    bool presentationStarted = false;
    std::uint64_t startupAtNs = 0;

    // Presentation pacing, logged once when playback ends: how many pictures
    // were shown, how many were decoded too late to be shown, the longest
    // pause between two pictures, and how often the decoder had nothing ready.
    struct Pacing {
        std::uint64_t firstNs = 0;
        std::uint64_t lastNs = 0;
        std::uint64_t longestGapNs = 0;
        std::uint64_t longestFirstSecondDtNs = 0;
        std::size_t presented = 0;
        std::size_t presentedFirstSecond = 0;
        std::size_t superseded = 0;
        std::size_t starved = 0;
        std::size_t starvedFirstSecond = 0;
        bool logged = false;
    } pacing;

    mutable std::mutex mutex;
    std::condition_variable queueChanged;
    std::deque<DecodedVideoFrame> videoQueue;
    std::deque<DecodedAudioFrame> audioQueue;
    std::size_t videoQueueBytes = 0;
    std::size_t audioQueueBytes = 0;
    std::thread worker;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> metadataReady{false};
    std::atomic<bool> decoderFinished{false};
    std::atomic<bool> decoderFailed{false};
    std::string error;
};

namespace {
void LogPacing(FfmpegState& state) {
    auto& pacing = state.pacing;
    if (pacing.logged || pacing.presented == 0) return;
    pacing.logged = true;
    const double seconds =
        static_cast<double>(pacing.lastNs - pacing.firstNs) / kNsPerSecond;
    PX_LOG_INFO(
        "VideoPlayer: presented {} frames in {:.2f}s ({:.1f} fps, {} in the first second), "
        "{} decoded too late, longest gap {:.1f} ms, decoder behind on {} updates "
        "({} in the first second), longest first-second update {:.1f} ms",
        pacing.presented, seconds,
        seconds > 0.0 ? static_cast<double>(pacing.presented - 1) / seconds : 0.0,
        pacing.presentedFirstSecond, pacing.superseded,
        static_cast<double>(pacing.longestGapNs) / 1'000'000.0, pacing.starved,
        pacing.starvedFirstSecond,
        static_cast<double>(pacing.longestFirstSecondDtNs) / 1'000'000.0);
}

// Live players, for app lifecycle suspend. Players are created, destroyed
// and suspended on the thread that owns the renderer.
std::mutex g_playersMutex;
std::vector<VideoPlayer*> g_players;
}  // namespace

VideoPlayer::VideoPlayer(SDL_Renderer* renderer, io::VFS& vfs)
    : m_renderer(renderer), m_vfs(vfs) {
    std::scoped_lock lock(g_playersMutex);
    g_players.push_back(this);
}

VideoPlayer::~VideoPlayer() {
    {
        std::scoped_lock lock(g_playersMutex);
        std::erase(g_players, this);
    }
    Close();
}

void VideoPlayer::SetAllSuspended(const bool suspended) {
    std::scoped_lock lock(g_playersMutex);
    for (VideoPlayer* player : g_players) {
        if (suspended) {
            if (!player->m_lifecycleSuspended && player->Pause())
                player->m_lifecycleSuspended = true;
        } else if (player->m_lifecycleSuspended) {
            player->m_lifecycleSuspended = false;
            (void)player->Resume();
        }
    }
}

int VideoPlayer::ReadFfmpeg(void* opaque, std::uint8_t* buffer, const int size) {
    auto* player = static_cast<VideoPlayer*>(opaque);
    if (!player || !player->m_ffmpeg || size <= 0) return AVERROR(EINVAL);
    auto& state = *player->m_ffmpeg;
    if (state.stopRequested.load()) return AVERROR_EXIT;
    const std::size_t count =
        state.stream->Read(buffer, static_cast<std::size_t>(size));
    if (count == 0)
        return state.stream->Failed() ? AVERROR(EIO) : AVERROR_EOF;
    if (count > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        return AVERROR(EOVERFLOW);
    return static_cast<int>(count);
}

std::int64_t VideoPlayer::SeekFfmpeg(void* opaque, const std::int64_t offset,
                                     const int whence) {
    auto* player = static_cast<VideoPlayer*>(opaque);
    if (!player || !player->m_ffmpeg) return AVERROR(EINVAL);
    auto& state = *player->m_ffmpeg;
    if (state.stopRequested.load()) return AVERROR_EXIT;
    if (whence == AVSEEK_SIZE) {
        const auto size = state.stream->Size();
        return size <= static_cast<std::uint64_t>(
                           (std::numeric_limits<std::int64_t>::max)())
                   ? static_cast<std::int64_t>(size)
                   : AVERROR(EOVERFLOW);
    }
    io::SeekOrigin origin = io::SeekOrigin::Begin;
    switch (whence & ~AVSEEK_FORCE) {
        case SEEK_SET: origin = io::SeekOrigin::Begin; break;
        case SEEK_CUR: origin = io::SeekOrigin::Current; break;
        case SEEK_END: origin = io::SeekOrigin::End; break;
        default: return AVERROR(EINVAL);
    }
    if (!state.stream->Seek(offset, origin)) return AVERROR(EINVAL);
    const auto position = state.stream->Tell();
    return position <= static_cast<std::uint64_t>(
                           (std::numeric_limits<std::int64_t>::max)())
               ? static_cast<std::int64_t>(position)
               : AVERROR(EOVERFLOW);
}

int VideoPlayer::InterruptFfmpeg(void* opaque) {
    const auto* player = static_cast<const VideoPlayer*>(opaque);
    return player && player->m_ffmpeg &&
                   player->m_ffmpeg->stopRequested.load()
               ? 1
               : 0;
}

bool VideoPlayer::Open(const std::string& vfsPath, const float volume) {
    Close();
    m_volume = std::clamp(volume, 0.0f, 1.0f);
    m_lastError.clear();
    m_clock = 0.0;
    m_ffmpeg = std::make_unique<FfmpegState>();
    if (PrepareFfmpeg(vfsPath, m_lastError) && CreateOutput(m_lastError)) {
        m_ffmpeg->metadataReady.store(true);
        if (StartWorker(vfsPath, false)) return true;
    }
    Stop();
    if (m_lastError.empty()) m_lastError = "media could not be opened";
    m_state.store(PlaybackState::Failed);
    return false;
}

bool VideoPlayer::OpenAsync(const std::string& vfsPath, const float volume) {
    const std::uint64_t startedAtNs = SDL_GetTicksNS();
    Close();
    m_volume = std::clamp(volume, 0.0f, 1.0f);
    m_ffmpeg = std::make_unique<FfmpegState>();
    if (StartWorker(vfsPath, true)) {
        PX_LOG_INFO("VideoPlayer: queued '{}' openMs={:.2f}", vfsPath,
                    static_cast<double>(SDL_GetTicksNS() - startedAtNs) / 1'000'000.0);
        return true;
    }
    CloseFfmpeg();
    m_state.store(PlaybackState::Failed);
    return false;
}

bool VideoPlayer::StartWorker(const std::string& vfsPath, const bool prepare) {
    m_state.store(PlaybackState::Playing);
    try {
        m_ffmpeg->worker = std::thread([this, vfsPath, prepare] {
            auto& state = *m_ffmpeg;
            if (prepare) {
                std::string error;
                if (!PrepareFfmpeg(vfsPath, error)) {
                    {
                        std::scoped_lock lock(state.mutex);
                        state.error = std::move(error);
                    }
                    state.decoderFailed.store(true);
                    state.decoderFinished.store(true);
                    state.queueChanged.notify_all();
                    return;
                }
                state.metadataReady.store(true);
            }
            PX_LOG_INFO("VideoPlayer: streaming '{}' ({}x{}, videoQueue={}, audioQueue={})",
                        vfsPath, state.width, state.height, kVideoQueueCapacity,
                        kAudioQueueCapacity);
            DecodeFfmpegLoop();
        });
    } catch (const std::exception& exception) {
        m_lastError = std::string("video decode worker could not start: ") +
                      exception.what();
        return false;
    }
    return true;
}

bool VideoPlayer::PrepareFfmpeg(const std::string& vfsPath, std::string& error) {
    auto stream = m_vfs.Open(vfsPath);
    if (!stream) {
        error = "media was not found or failed integrity validation";
        PX_LOG_WARN("VideoPlayer: {} '{}'", error, vfsPath);
        return false;
    }

    auto& state = *m_ffmpeg;
    state.stream = std::move(stream);
    state.format = avformat_alloc_context();
    auto* ioBuffer = static_cast<unsigned char*>(av_malloc(64 * 1024));
    if (!state.format || !ioBuffer) {
        error = "FFmpeg allocation failed";
        if (ioBuffer) av_free(ioBuffer);
        return false;
    }
    state.io = avio_alloc_context(ioBuffer, 64 * 1024, 0, this,
                                  &VideoPlayer::ReadFfmpeg, nullptr,
                                  &VideoPlayer::SeekFfmpeg);
    if (!state.io) {
        av_free(ioBuffer);
        error = "FFmpeg AVIO allocation failed";
        return false;
    }
    state.format->pb = state.io;
    state.format->flags |= AVFMT_FLAG_CUSTOM_IO;
    state.format->interrupt_callback = {&VideoPlayer::InterruptFfmpeg, this};

    int result = avformat_open_input(&state.format, nullptr, nullptr, nullptr);
    if (result >= 0) result = avformat_find_stream_info(state.format, nullptr);
    if (result < 0) {
        error = "FFmpeg could not inspect media: " + AvError(result);
        PX_LOG_WARN("VideoPlayer: '{}' ({})", vfsPath, error);
        return false;
    }

    const AVCodec* videoDecoder = nullptr;
    state.videoStream = av_find_best_stream(
        state.format, AVMEDIA_TYPE_VIDEO, -1, -1, &videoDecoder, 0);
    if (state.videoStream < 0 || !videoDecoder) {
        error = "media has no supported video stream";
        return false;
    }
    state.videoCodec = avcodec_alloc_context3(videoDecoder);
    const AVCodecParameters* videoParameters =
        state.format->streams[state.videoStream]->codecpar;
    // FFmpeg decodes on one thread unless asked; let it use the machine's
    // cores so heavier codecs and resolutions keep up with playback.
    if (state.videoCodec) state.videoCodec->thread_count = 0;
    if (!state.videoCodec ||
        avcodec_parameters_to_context(state.videoCodec, videoParameters) < 0 ||
        avcodec_open2(state.videoCodec, videoDecoder, nullptr) < 0) {
        error = "video codec could not be opened";
        return false;
    }
    state.videoCodec->err_recognition = AV_EF_CAREFUL;

    state.width = state.videoCodec->width;
    state.height = state.videoCodec->height;
    if (state.width <= 0 || state.height <= 0 || state.width > 16'384 ||
        state.height > 16'384 ||
        static_cast<std::uint64_t>(state.width) *
                static_cast<std::uint64_t>(state.height) * 3u >
            kVideoQueueByteCapacity) {
        error = "video dimensions exceed runtime limits";
        return false;
    }
    state.videoFrame = av_frame_alloc();
    state.scaler = sws_getContext(
        state.width, state.height, state.videoCodec->pix_fmt, state.width, state.height,
        AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!state.videoFrame || !state.scaler) {
        error = "video frame resources could not be created";
        return false;
    }

    const AVRational guessedRate = av_guess_frame_rate(
        state.format, state.format->streams[state.videoStream], nullptr);
    if (guessedRate.num > 0 && guessedRate.den > 0) {
        const double rate = av_q2d(guessedRate);
        if (std::isfinite(rate) && rate > 0.0 && rate <= 1'000.0)
            state.videoFrameDuration = 1.0 / rate;
    }

    const AVCodec* audioDecoder = nullptr;
    state.audioStream = av_find_best_stream(
        state.format, AVMEDIA_TYPE_AUDIO, -1, state.videoStream, &audioDecoder,
        0);
    if (state.audioStream >= 0 && audioDecoder) {
        state.audioCodec = avcodec_alloc_context3(audioDecoder);
        const auto* parameters =
            state.format->streams[state.audioStream]->codecpar;
        if (!state.audioCodec ||
            avcodec_parameters_to_context(state.audioCodec, parameters) < 0 ||
            avcodec_open2(state.audioCodec, audioDecoder, nullptr) < 0) {
            avcodec_free_context(&state.audioCodec);
            state.audioStream = -1;
        } else {
            state.audioCodec->err_recognition = AV_EF_CAREFUL;
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            result = swr_alloc_set_opts2(
                &state.resampler, &stereo, AV_SAMPLE_FMT_FLT,
                kOutputSampleRate, &state.audioCodec->ch_layout,
                state.audioCodec->sample_fmt, state.audioCodec->sample_rate, 0,
                nullptr);
            if (result < 0 || swr_init(state.resampler) < 0) {
                swr_free(&state.resampler);
                avcodec_free_context(&state.audioCodec);
                state.audioStream = -1;
            } else {
                state.audioFrame = av_frame_alloc();
                if (!state.audioFrame) {
                    swr_free(&state.resampler);
                    avcodec_free_context(&state.audioCodec);
                    state.audioStream = -1;
                }
            }
        }
    }

    state.timeOrigin = StreamStartSeconds(state.format);
    return !state.stopRequested.load();
}

bool VideoPlayer::CreateOutput(std::string& error) {
    auto& state = *m_ffmpeg;
    m_width = state.width;
    m_height = state.height;
    m_texture = graphics::TextureResource::CreateStreaming(
        graphics::TextureBackend::SdlRenderer, m_renderer,
        graphics::StreamingTextureFormat::Rgb24, m_width, m_height);
    if (!m_texture) {
        error = "video texture could not be created";
        return false;
    }
    (void)m_texture.SetLinearSampling();
    if (state.audioCodec) {
        SDL_AudioSpec specification{};
        specification.format = SDL_AUDIO_F32;
        specification.channels = kOutputChannels;
        specification.freq = kOutputSampleRate;
        m_audio = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &specification, nullptr, nullptr);
        if (!m_audio)
            PX_LOG_WARN(
                "VideoPlayer: audio output is unavailable; continuing silently: {}",
                SDL_GetError());
    }
    state.outputCreated = true;
    state.startupAtNs = SDL_GetTicksNS();
    return true;
}

void VideoPlayer::DecodeFfmpegLoop() {
    auto& state = *m_ffmpeg;
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        std::scoped_lock lock(state.mutex);
        state.error = "FFmpeg packet allocation failed";
        state.decoderFailed.store(true);
        state.decoderFinished.store(true);
        state.queueChanged.notify_all();
        return;
    }

    const auto fail = [&](std::string error) {
        {
            std::scoped_lock lock(state.mutex);
            state.error = std::move(error);
        }
        state.decoderFailed.store(true);
    };
    const auto framePts = [&](const AVFrame* frame, const int streamIndex,
                              double& fallback, const double duration) {
        const auto timestamp = frame->best_effort_timestamp;
        double pts = fallback;
        if (timestamp != AV_NOPTS_VALUE) {
            pts = static_cast<double>(timestamp) *
                      av_q2d(state.format->streams[streamIndex]->time_base) -
                  state.timeOrigin;
        }
        if (!std::isfinite(pts)) pts = fallback;
        pts = std::max(0.0, pts);
        fallback = std::max(fallback, pts + std::max(0.0, duration));
        return pts;
    };
    const auto pushVideo = [&](DecodedVideoFrame decoded) {
        std::unique_lock lock(state.mutex);
        const std::size_t bytes = decoded.rgb.size();
        state.queueChanged.wait(lock, [&] {
            return state.stopRequested.load() ||
                   (state.videoQueue.size() < kVideoQueueCapacity &&
                    bytes <= kVideoQueueByteCapacity - state.videoQueueBytes);
        });
        if (state.stopRequested.load()) return false;
        state.videoQueueBytes += bytes;
        state.videoQueue.push_back(std::move(decoded));
        state.queueChanged.notify_all();
        return true;
    };
    const auto pushAudio = [&](DecodedAudioFrame decoded) {
        const std::size_t bytes = decoded.samples.size() * sizeof(float);
        if (bytes > kAudioQueueByteCapacity) {
            fail("decoded audio frame exceeds queue memory budget");
            return false;
        }
        // Every frame of a packet already sent is kept, whatever the frame
        // count: one packet can hold more than a second of audio, and waiting
        // here for room would stop video decoding until playback consumed it.
        // The packet loop sends audio only while the queue has room, and the
        // byte budget still bounds memory.
        std::unique_lock lock(state.mutex);
        state.queueChanged.wait(lock, [&] {
            return state.stopRequested.load() ||
                   bytes <= kAudioQueueByteCapacity - state.audioQueueBytes;
        });
        if (state.stopRequested.load()) return false;
        state.audioQueueBytes += bytes;
        state.audioQueue.push_back(std::move(decoded));
        state.queueChanged.notify_all();
        return true;
    };
    const auto drainVideo = [&]() {
        for (;;) {
            const int received =
                avcodec_receive_frame(state.videoCodec, state.videoFrame);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
                return true;
            if (received < 0) {
                if (received == AVERROR_INVALIDDATA) continue;
                fail("video decode failed: " + AvError(received));
                return false;
            }
            if (state.videoFrame->width != state.width ||
                state.videoFrame->height != state.height) {
                av_frame_unref(state.videoFrame);
                fail("mid-stream video resolution changes are unsupported");
                return false;
            }
            DecodedVideoFrame decoded;
            decoded.pts = framePts(state.videoFrame, state.videoStream,
                                   state.nextVideoPts,
                                   state.videoFrameDuration);
            decoded.rgb.resize(static_cast<std::size_t>(state.width) *
                               static_cast<std::size_t>(state.height) * 3u);
            std::uint8_t* destination[] = {decoded.rgb.data(), nullptr, nullptr,
                                           nullptr};
            const int strides[] = {state.width * 3, 0, 0, 0};
            const int scaled = sws_scale(
                state.scaler, state.videoFrame->data,
                state.videoFrame->linesize, 0, state.height, destination, strides);
            av_frame_unref(state.videoFrame);
            if (scaled != state.height) {
                fail("video color conversion failed");
                return false;
            }
            if (!pushVideo(std::move(decoded))) return false;
        }
    };
    const auto drainAudio = [&]() {
        if (!state.audioCodec || !state.audioFrame || !state.resampler)
            return true;
        for (;;) {
            const int received =
                avcodec_receive_frame(state.audioCodec, state.audioFrame);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
                return true;
            if (received < 0) {
                if (received == AVERROR_INVALIDDATA) continue;
                fail("audio decode failed: " + AvError(received));
                return false;
            }
            const int capacity = swr_get_out_samples(
                state.resampler, state.audioFrame->nb_samples);
            if (capacity <= 0 || capacity > kOutputSampleRate * 8) {
                av_frame_unref(state.audioFrame);
                continue;
            }
            DecodedAudioFrame decoded;
            decoded.samples.resize(static_cast<std::size_t>(capacity) *
                                   kOutputChannels);
            std::uint8_t* output[] = {
                reinterpret_cast<std::uint8_t*>(decoded.samples.data())};
            const int converted = swr_convert(
                state.resampler, output, capacity,
                const_cast<const std::uint8_t**>(
                    state.audioFrame->extended_data),
                state.audioFrame->nb_samples);
            const double duration = converted > 0
                                        ? static_cast<double>(converted) /
                                              kOutputSampleRate
                                        : 0.0;
            // Decoded audio is timed by the samples before it, not by the
            // frame's own timestamp: decoders such as WMA Lossless stamp
            // their first frames with a later packet's time (1.3 s late in
            // one game's opening movie), which made playback start that far
            // in and throw the first second of pictures away.
            decoded.pts = state.audioStartPts +
                          static_cast<double>(state.audioSamplesDecoded) /
                              kOutputSampleRate;
            if (converted > 0)
                state.audioSamplesDecoded += static_cast<std::uint64_t>(converted);
            decoded.duration = duration;
            av_frame_unref(state.audioFrame);
            if (converted <= 0) continue;
            decoded.samples.resize(static_cast<std::size_t>(converted) *
                                   kOutputChannels);
            if (m_volume < 0.999f) {
                for (float& sample : decoded.samples) sample *= m_volume;
            }
            if (!pushAudio(std::move(decoded))) return false;
        }
    };

    const auto decodeVideo = [&](AVPacket* source) {
        bool ok = true;
        int sent = avcodec_send_packet(state.videoCodec, source);
        if (sent == AVERROR(EAGAIN)) {
            ok = drainVideo();
            if (ok) sent = avcodec_send_packet(state.videoCodec, source);
        }
        if (sent == AVERROR_INVALIDDATA) return true;
        if (sent < 0) {
            fail("video packet submission failed: " + AvError(sent));
            return false;
        }
        return ok && drainVideo();
    };
    const auto decodeAudio = [&](AVPacket* source) {
        if (state.audioStartPts < 0.0) {
            const auto timestamp =
                source->pts != AV_NOPTS_VALUE ? source->pts : source->dts;
            const double start =
                timestamp == AV_NOPTS_VALUE
                    ? 0.0
                    : static_cast<double>(timestamp) *
                              av_q2d(state.format->streams[state.audioStream]->time_base) -
                          state.timeOrigin;
            state.audioStartPts = std::isfinite(start) ? std::max(0.0, start) : 0.0;
        }
        bool ok = true;
        int sent = avcodec_send_packet(state.audioCodec, source);
        if (sent == AVERROR(EAGAIN)) {
            ok = drainAudio();
            if (ok) sent = avcodec_send_packet(state.audioCodec, source);
        }
        if (sent == AVERROR_INVALIDDATA) return true;
        if (sent < 0) {
            fail("audio packet submission failed: " + AvError(sent));
            return false;
        }
        return ok && drainAudio();
    };
    const auto videoHasRoom = [&] {
        std::scoped_lock lock(state.mutex);
        return state.videoQueue.size() < kVideoQueueCapacity;
    };
    const auto audioHasRoom = [&] {
        std::scoped_lock lock(state.mutex);
        return state.audioQueue.size() < kAudioQueueCapacity;
    };
    // Packets wait here instead of blocking the demuxer on a full frame queue.
    struct Pending {
        std::deque<AVPacket*> packets;
        std::size_t bytes = 0;
        std::size_t packetCapacity = 0;
        std::size_t byteCapacity = 0;
    };
    Pending pendingVideo{.packetCapacity = kPendingVideoPacketCapacity,
                         .byteCapacity = kPendingVideoByteCapacity};
    Pending pendingAudio{.packetCapacity = kPendingAudioPacketCapacity,
                         .byteCapacity = kPendingAudioByteCapacity};
    const auto decodeFront = [&](Pending& pending, const auto& decode) {
        AVPacket* next = pending.packets.front();
        pending.packets.pop_front();
        pending.bytes -= static_cast<std::size_t>(next->size);
        const bool ok = decode(next);
        av_packet_free(&next);
        return ok;
    };
    const auto decodePending = [&](Pending& pending, const auto& hasRoom,
                                   const auto& decode) {
        while (!pending.packets.empty() && !state.stopRequested.load() && hasRoom())
            if (!decodeFront(pending, decode)) return false;
        return true;
    };
    // Decodes the packet now when its queue has room and nothing waits before
    // it; otherwise holds it, decoding the oldest held packets first (waiting
    // for room as before) when the hold is full.
    const auto submit = [&](Pending& pending, const auto& hasRoom,
                            const auto& decode, AVPacket* source) {
        if (pending.packets.empty() && hasRoom()) return decode(source);
        bool ok = true;
        while (ok && !pending.packets.empty() &&
               (pending.packets.size() >= pending.packetCapacity ||
                pending.bytes + static_cast<std::size_t>(source->size) >
                    pending.byteCapacity))
            ok = decodeFront(pending, decode);
        if (!ok) return false;
        AVPacket* held = av_packet_alloc();
        if (!held) {
            fail("FFmpeg packet allocation failed");
            return false;
        }
        av_packet_move_ref(held, source);
        pending.bytes += static_cast<std::size_t>(held->size);
        pending.packets.push_back(held);
        return true;
    };

    const bool hasAudio = state.audioCodec != nullptr;
    int corruptPackets = 0;
    while (!state.stopRequested.load()) {
        if (!decodePending(pendingVideo, videoHasRoom, decodeVideo)) break;
        if (!decodePending(pendingAudio, audioHasRoom, decodeAudio)) break;
        {
            // Read on only while a queue has room: the stream with room then
            // gets its packets even when the file stores them far behind,
            // and nothing is read ahead while both are full.
            std::unique_lock lock(state.mutex);
            state.queueChanged.wait(lock, [&] {
                return state.stopRequested.load() ||
                       state.videoQueue.size() < kVideoQueueCapacity ||
                       (hasAudio && state.audioQueue.size() < kAudioQueueCapacity);
            });
        }
        if (state.stopRequested.load()) break;
        const int read = av_read_frame(state.format, packet);
        if (read >= 0) {
            corruptPackets = 0;
            bool ok = true;
            if (packet->stream_index == state.videoStream)
                ok = submit(pendingVideo, videoHasRoom, decodeVideo, packet);
            else if (packet->stream_index == state.audioStream && hasAudio)
                ok = submit(pendingAudio, audioHasRoom, decodeAudio, packet);
            av_packet_unref(packet);
            if (!ok) break;
            continue;
        }
        if (read == AVERROR_INVALIDDATA &&
            ++corruptPackets <= kCorruptPacketLimit)
            continue;
        if (read != AVERROR_EOF && read != AVERROR_EXIT) {
            fail("media demux failed: " + AvError(read));
            break;
        }
        if (read == AVERROR_EXIT || state.stopRequested.load()) break;
        // At the end, decode what is held as each stream's queue makes room,
        // so neither waits for all of the other's to be played.
        bool ok = true;
        while (ok && !state.stopRequested.load() &&
               (!pendingVideo.packets.empty() || !pendingAudio.packets.empty())) {
            if (!pendingVideo.packets.empty() && videoHasRoom()) {
                ok = decodeFront(pendingVideo, decodeVideo);
            } else if (!pendingAudio.packets.empty() && audioHasRoom()) {
                ok = decodeFront(pendingAudio, decodeAudio);
            } else {
                std::unique_lock lock(state.mutex);
                state.queueChanged.wait(lock, [&] {
                    return state.stopRequested.load() ||
                           (!pendingVideo.packets.empty() &&
                            state.videoQueue.size() < kVideoQueueCapacity) ||
                           (!pendingAudio.packets.empty() &&
                            state.audioQueue.size() < kAudioQueueCapacity);
                });
            }
        }
        if (!ok || state.stopRequested.load()) break;
        (void)avcodec_send_packet(state.videoCodec, nullptr);
        if (!drainVideo()) break;
        if (hasAudio) {
            (void)avcodec_send_packet(state.audioCodec, nullptr);
            if (!drainAudio()) break;
        }
        break;
    }
    for (AVPacket* held : pendingVideo.packets) av_packet_free(&held);
    for (AVPacket* held : pendingAudio.packets) av_packet_free(&held);
    av_packet_free(&packet);
    state.decoderFinished.store(true);
    state.queueChanged.notify_all();
}

void VideoPlayer::UpdateFfmpeg(const float dt) {
    if (!m_ffmpeg || m_state.load() != PlaybackState::Playing) return;
    auto& state = *m_ffmpeg;
    if (state.decoderFailed.load()) {
        std::scoped_lock lock(state.mutex);
        m_lastError = state.error.empty() ? "video decode failed" : state.error;
        m_state.store(PlaybackState::Failed);
        PX_LOG_ERROR("VideoPlayer: {}", m_lastError);
        return;
    }
    if (!state.metadataReady.load()) return;
    if (!state.outputCreated && !CreateOutput(m_lastError)) {
        state.stopRequested.store(true);
        state.queueChanged.notify_all();
        m_state.store(PlaybackState::Failed);
        return;
    }

    const int bytesPerSecond =
        kOutputSampleRate * kOutputChannels * static_cast<int>(sizeof(float));
    const bool starting = !state.presentationStarted;
    if (starting) {
        std::unique_lock lock(state.mutex);
        const bool videoReady = state.videoQueue.size() >= kStartupVideoFrames;
        const bool audioReady = !m_audio ||
            static_cast<double>(state.audioQueueBytes) / bytesPerSecond >=
                kStartupAudioSeconds;
        // Probing/buffering time is not playback time. Keep audio paused until
        // both streams are ready, and never apply the incoming frame's dt to
        // the first picture. EOF and a bounded timeout cover short/poorly
        // interleaved media without leaving startup waiting forever.
        if (!(videoReady && audioReady) && !state.decoderFinished.load() &&
            SDL_GetTicksNS() - state.startupAtNs < kMaximumStartupWaitNs)
            return;
        state.presentationStarted = true;
        const std::size_t bufferedFrames = state.videoQueue.size();
        const double bufferedAudioSeconds =
            static_cast<double>(state.audioQueueBytes) / bytesPerSecond;
        lock.unlock();
        PX_LOG_INFO("VideoPlayer: playback ready bufferMs={:.2f} videoFrames={} audioSeconds={:.3f}",
                    static_cast<double>(SDL_GetTicksNS() - state.startupAtNs) / 1'000'000.0,
                    bufferedFrames, bufferedAudioSeconds);
    } else {
        m_clock += std::clamp(static_cast<double>(dt), 0.0, 0.25);
    }
    int queuedAudioBytes = m_audio ? SDL_GetAudioStreamQueued(m_audio) : 0;
    if (queuedAudioBytes < 0) queuedAudioBytes = 0;
    for (;;) {
        DecodedAudioFrame decoded;
        {
            std::scoped_lock lock(state.mutex);
            if (state.audioQueue.empty()) break;
            const double queuedSeconds =
                static_cast<double>(queuedAudioBytes) / bytesPerSecond;
            if (m_audio && queuedSeconds >= kAudioQueueLeadSeconds) break;
            decoded = std::move(state.audioQueue.front());
            state.audioQueueBytes -= decoded.samples.size() * sizeof(float);
            state.audioQueue.pop_front();
        }
        state.queueChanged.notify_all();
        if (m_audio && !decoded.samples.empty()) {
            const std::size_t byteCount = decoded.samples.size() * sizeof(float);
            if (byteCount <= static_cast<std::size_t>(
                                 (std::numeric_limits<int>::max)()) &&
                SDL_PutAudioStreamData(m_audio, decoded.samples.data(),
                                       static_cast<int>(byteCount))) {
                queuedAudioBytes += static_cast<int>(byteCount);
                state.audioSubmittedEndPts =
                    std::max(state.audioSubmittedEndPts,
                             decoded.pts + decoded.duration);
                state.audioStarted = true;
            }
        }
    }

    double masterClock = m_clock;
    if (m_audio && state.audioStarted) {
        queuedAudioBytes = std::max(0, SDL_GetAudioStreamQueued(m_audio));
        const double audioClock =
            state.audioSubmittedEndPts -
            static_cast<double>(queuedAudioBytes) / bytesPerSecond;
        // The device is the clock while it has samples. When it runs dry,
        // wall time carries on, so a picture never waits on audio that has
        // not been decoded yet.
        if (queuedAudioBytes > 0 && std::isfinite(audioClock)) {
            masterClock = std::max(0.0, audioClock);
            m_clock = masterClock;
        }
    }

    std::vector<std::uint8_t> latestFrame;
    auto& pacing = state.pacing;
    const std::uint64_t nowNs = SDL_GetTicksNS();
    const bool firstSecond =
        pacing.presented == 0 || nowNs - pacing.firstNs < kNsPerSecond;
    if (firstSecond && !starting)
        pacing.longestFirstSecondDtNs =
            std::max(pacing.longestFirstSecondDtNs,
                     static_cast<std::uint64_t>(std::max(0.0f, dt) * 1e9f));
    {
        std::scoped_lock lock(state.mutex);
        std::size_t popped = 0;
        while (!state.videoQueue.empty() &&
               state.videoQueue.front().pts <=
                   masterClock + kVideoEarlyToleranceSeconds) {
            latestFrame = std::move(state.videoQueue.front().rgb);
            state.videoQueueBytes -= latestFrame.size();
            state.videoQueue.pop_front();
            ++popped;
        }
        if (popped > 1) pacing.superseded += popped - 1;
        if (state.videoQueue.empty() && !state.decoderFinished.load()) {
            ++pacing.starved;
            if (firstSecond) ++pacing.starvedFirstSecond;
        }
    }
    if (!latestFrame.empty()) {
        (void)m_texture.Update(latestFrame.data(),
                               static_cast<std::size_t>(m_width) * 3u);
        state.queueChanged.notify_all();
        if (pacing.presented == 0) pacing.firstNs = nowNs;
        else pacing.longestGapNs = std::max(pacing.longestGapNs, nowNs - pacing.lastNs);
        pacing.lastNs = nowNs;
        ++pacing.presented;
        if (nowNs - pacing.firstNs < kNsPerSecond) ++pacing.presentedFirstSecond;
    }
    if (starting && m_audio) (void)SDL_ResumeAudioStreamDevice(m_audio);

    if (state.decoderFailed.load()) {
        std::scoped_lock lock(state.mutex);
        m_lastError = state.error.empty() ? "video decode failed" : state.error;
        m_state.store(PlaybackState::Failed);
        PX_LOG_ERROR("VideoPlayer: {}", m_lastError);
        return;
    }

    bool queuesEmpty = false;
    {
        std::scoped_lock lock(state.mutex);
        queuesEmpty = state.videoQueue.empty() && state.audioQueue.empty();
    }
    queuedAudioBytes =
        m_audio ? std::max(0, SDL_GetAudioStreamQueued(m_audio)) : 0;
    if (state.decoderFinished.load() && queuesEmpty && queuedAudioBytes == 0) {
        LogPacing(state);
        m_state.store(PlaybackState::Finished);
    }
}

void VideoPlayer::Update(const float dt) { UpdateFfmpeg(dt); }

bool VideoPlayer::Pause() {
    if (m_state.load() != PlaybackState::Playing) return false;
    if (m_audio) (void)SDL_PauseAudioStreamDevice(m_audio);
    m_state.store(PlaybackState::Paused);
    return true;
}

bool VideoPlayer::Resume() {
    if (m_state.load() != PlaybackState::Paused) return false;
    if (m_audio && m_ffmpeg && m_ffmpeg->presentationStarted)
        (void)SDL_ResumeAudioStreamDevice(m_audio);
    m_state.store(PlaybackState::Playing);
    return true;
}

void VideoPlayer::Stop() {
    CloseFfmpeg();
    if (m_audio) {
        SDL_DestroyAudioStream(m_audio);
        m_audio = nullptr;
    }
    m_texture.Reset();
    m_width = 0;
    m_height = 0;
    m_clock = 0.0;
    m_state.store(PlaybackState::Stopped);
}

void VideoPlayer::Skip() { Stop(); }

void VideoPlayer::CloseFfmpeg() {
    if (!m_ffmpeg) return;
    auto& state = *m_ffmpeg;
    LogPacing(state);
    state.stopRequested.store(true);
    state.queueChanged.notify_all();
    if (state.worker.joinable()) state.worker.join();
    if (state.scaler) sws_freeContext(state.scaler);
    if (state.resampler) swr_free(&state.resampler);
    if (state.audioFrame) av_frame_free(&state.audioFrame);
    if (state.videoFrame) av_frame_free(&state.videoFrame);
    if (state.videoCodec) avcodec_free_context(&state.videoCodec);
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
    m_width = 0;
    m_height = 0;
    m_clock = 0.0;
    m_lastError.clear();
    m_state.store(PlaybackState::Idle);
}

std::string VideoPlayer::LastError() const {
    if (!m_ffmpeg) return m_lastError;
    std::scoped_lock lock(m_ffmpeg->mutex);
    return m_ffmpeg->error.empty() ? m_lastError : m_ffmpeg->error;
}

QueueMetrics VideoPlayer::Queues() const {
    QueueMetrics metrics{.videoCapacity = kVideoQueueCapacity,
                         .audioCapacity = kAudioQueueCapacity,
                         .videoByteCapacity = kVideoQueueByteCapacity,
                         .audioByteCapacity = kAudioQueueByteCapacity};
    if (!m_ffmpeg) return metrics;
    std::scoped_lock lock(m_ffmpeg->mutex);
    metrics.videoFrames = m_ffmpeg->videoQueue.size();
    metrics.audioFrames = m_ffmpeg->audioQueue.size();
    metrics.videoBytes = m_ffmpeg->videoQueueBytes;
    metrics.audioBytes = m_ffmpeg->audioQueueBytes;
    return metrics;
}

void VideoPlayer::Render(const int logicalW, const int logicalH) {
    if (!m_texture || !m_ffmpeg || !m_ffmpeg->presentationStarted ||
        m_width <= 0 || m_height <= 0) return;
    SDL_FRect destination;
    const float scale = std::min(static_cast<float>(logicalW) / m_width,
                                 static_cast<float>(logicalH) / m_height);
    destination.w = m_width * scale;
    destination.h = m_height * scale;
    destination.x = (logicalW - destination.w) * 0.5f;
    destination.y = (logicalH - destination.h) * 0.5f;
    SDL_RenderTexture(
        m_renderer,
        static_cast<SDL_Texture*>(
            m_texture.Native(graphics::TextureBackend::SdlRenderer)),
        nullptr, &destination);
}

}  // namespace px::video
