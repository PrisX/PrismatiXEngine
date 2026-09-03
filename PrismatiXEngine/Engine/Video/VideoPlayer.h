#pragma once

#include "Engine/Graphics/Texture.h"
#include "Engine/IO/VFS.h"

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <memory>
#include <string>

struct SDL_Renderer;
struct SDL_AudioStream;
namespace px::video {

struct FfmpegState;

enum class PlaybackState : std::uint8_t {
    Idle,
    Playing,
    Paused,
    Finished,
    Stopped,
    Failed,
};

struct QueueMetrics {
    std::size_t videoFrames = 0;
    std::size_t audioFrames = 0;
    std::size_t videoBytes = 0;
    std::size_t audioBytes = 0;
    std::size_t videoCapacity = 0;
    std::size_t audioCapacity = 0;
    std::size_t videoByteCapacity = 0;
    std::size_t audioByteCapacity = 0;
};

// Native FFmpeg player. Demux/codec/resample work runs on a bounded worker;
// SDL texture and audio-device calls remain on the owning render thread.
class VideoPlayer {
public:
    VideoPlayer(SDL_Renderer* renderer, io::VFS& vfs);
    ~VideoPlayer();

    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    bool Open(const std::string& vfsPath, float volume = 1.0f);
    void Close();
    bool Pause();
    bool Resume();
    void Stop();
    void Skip();

    void Update(float dt);
    // Draws letterboxed into the given logical-coordinate viewport.
    void Render(int logicalW, int logicalH);

    [[nodiscard]] bool Playing() const {
        const auto state = m_state.load();
        return state == PlaybackState::Playing || state == PlaybackState::Paused;
    }
    [[nodiscard]] bool Paused() const { return m_state.load() == PlaybackState::Paused; }
    [[nodiscard]] bool Finished() const { return m_state.load() == PlaybackState::Finished; }
    [[nodiscard]] PlaybackState State() const { return m_state.load(); }
    [[nodiscard]] std::string LastError() const;
    [[nodiscard]] QueueMetrics Queues() const;
    [[nodiscard]] graphics::TextureHandle Texture() const {
        return m_texture.Handle();
    }
    [[nodiscard]] int Width() const { return m_width; }
    [[nodiscard]] int Height() const { return m_height; }

private:
    bool OpenFfmpeg(const std::string& vfsPath);
    void UpdateFfmpeg(float dt);
    void DecodeFfmpegLoop();
    void CloseFfmpeg();
    static int ReadFfmpeg(void* opaque, std::uint8_t* buffer, int size);
    static std::int64_t SeekFfmpeg(void* opaque, std::int64_t offset, int whence);
    static int InterruptFfmpeg(void* opaque);
    SDL_Renderer* m_renderer;
    io::VFS& m_vfs;

    graphics::TextureResource m_texture;
    int m_width = 0;
    int m_height = 0;

    SDL_AudioStream* m_audio = nullptr;
    float m_volume = 1.0f;
    double m_clock = 0.0;
    std::atomic<PlaybackState> m_state{PlaybackState::Idle};
    std::string m_lastError;
    std::unique_ptr<FfmpegState> m_ffmpeg;
};

}
