#pragma once

#include "Engine/Graphics/Texture.h"
#include "Engine/IO/VFS.h"

#include <cstdint>
#include <memory>
#include <string>

struct SDL_Renderer;
struct SDL_AudioStream;
typedef struct plm_t plm_t;

namespace px::video {

struct FfmpegState;

// MPEG-1 (.mpg) playback via pl_mpeg: video to an SDL texture, MP2 audio
// pushed to a dedicated SDL audio stream. Intended for OP/ED movies.
class VideoPlayer {
public:
    VideoPlayer(SDL_Renderer* renderer, io::VFS& vfs);
    ~VideoPlayer();

    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    bool Open(const std::string& vfsPath, float volume = 1.0f);
    void Close();

    void Update(float dt);
    // Draws letterboxed into the given logical-coordinate viewport.
    void Render(int logicalW, int logicalH);

    [[nodiscard]] bool Playing() const { return (m_plm != nullptr || m_ffmpeg) && !m_finished; }
    [[nodiscard]] bool Finished() const { return m_finished; }
    [[nodiscard]] graphics::TextureHandle Texture() const {
        return m_texture.Handle();
    }
    [[nodiscard]] int Width() const { return m_width; }
    [[nodiscard]] int Height() const { return m_height; }

    // Internal decoder callbacks (pl_mpeg types are not forward-declarable).
    void HandleVideoFrame(void* frame);
    void HandleAudioSamples(void* samples);

private:
    bool OpenFfmpeg(const std::string& vfsPath);
    void UpdateFfmpeg(float dt);
    bool DecodeNextFfmpegFrame();
    void DecodeFfmpegAudio(void* packet);
    void CloseFfmpeg();
    static int ReadFfmpeg(void* opaque, std::uint8_t* buffer, int size);
    static std::int64_t SeekFfmpeg(void* opaque, std::int64_t offset, int whence);
    SDL_Renderer* m_renderer;
    io::VFS& m_vfs;

    plm_t* m_plm = nullptr;
    io::Bytes m_data;
    graphics::TextureResource m_texture;
    std::unique_ptr<std::uint8_t[]> m_rgb;
    int m_width = 0;
    int m_height = 0;

    SDL_AudioStream* m_audio = nullptr;
    float m_volume = 1.0f;
    bool m_finished = false;
    std::unique_ptr<FfmpegState> m_ffmpeg;
};

}
