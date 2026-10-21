#if defined(PRISMATIX_WEB_PLAYER)
#include "Engine/Video/VideoPlayer.h"
#include <SDL3/SDL.h>
#include <emscripten/emscripten.h>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace px::video {
struct FfmpegState {};
namespace {
std::vector<VideoPlayer*> players;
EM_JS(int, OpenVideo, (const char* path, float volume), {
    return Module.prismatix.video.open(UTF8ToString(path), volume);
});
EM_JS(void, ControlVideo, (int handle, const char* operation), {
    Module.prismatix.video.control(handle, UTF8ToString(operation));
});
EM_JS(double, VideoValue, (int handle, const char* key), {
    return Module.prismatix.video.value(handle, UTF8ToString(key));
});
EM_JS(char*, VideoError, (int handle), {
    return stringToNewUTF8(Module.prismatix.video.error(handle));
});
#if defined(PRISMATIX_WEB_WEBGPU)
// WebGPU renderer stopgap: SDL_GPU exposes no WGPUTexture for an SDL_Texture,
// so each frame goes through a 2D canvas into SDL_UpdateTexture (a CPU copy,
// about half the WebGL2 frame rate for 1080p video). Zero-copy needs
// copyExternalImageToTexture on the backend's own texture; see
// docs/web-export.md (WebGPU status).
std::vector<std::uint8_t> webGpuFrame;
EM_JS(void, ReadVideo, (int handle, void* pixels, int width, int height), {
    const video = Module.prismatix.video.element(handle);
    const canvas = Module.prismatix.videoCanvas ??= new OffscreenCanvas(width, height);
    if (canvas.width !== width || canvas.height !== height) { canvas.width = width; canvas.height = height; }
    const context = canvas.getContext("2d", {willReadFrequently: true});
    context.drawImage(video, 0, 0, width, height);
    HEAPU8.set(context.getImageData(0, 0, width, height).data, pixels);
});
#else
EM_JS(void, UploadVideo, (int handle, int texture), {
    const video = Module.prismatix.video.element(handle);
    const previous = GLctx.getParameter(GLctx.TEXTURE_BINDING_2D);
    const flip = GLctx.getParameter(GLctx.UNPACK_FLIP_Y_WEBGL);
    GLctx.bindTexture(GLctx.TEXTURE_2D, GL.textures[texture]);
    GLctx.pixelStorei(GLctx.UNPACK_FLIP_Y_WEBGL, false);
    GLctx.texSubImage2D(GLctx.TEXTURE_2D, 0, 0, 0, GLctx.RGBA, GLctx.UNSIGNED_BYTE, video);
    GLctx.pixelStorei(GLctx.UNPACK_FLIP_Y_WEBGL, flip);
    GLctx.bindTexture(GLctx.TEXTURE_2D, previous);
});
#endif
} // namespace

VideoPlayer::VideoPlayer(SDL_Renderer* renderer, io::VFS& vfs)
    : m_renderer(renderer), m_vfs(vfs) { players.push_back(this); }
VideoPlayer::~VideoPlayer() {
    Close();
    std::erase(players, this);
}
bool VideoPlayer::Open(const std::string& path, const float volume) {
    return OpenAsync(path, volume);
}
bool VideoPlayer::OpenAsync(const std::string& path, const float volume) {
    Close();
    m_volume = std::clamp(volume, 0.0f, 1.0f);
    m_browserHandle = OpenVideo(path.c_str(), m_volume);
    m_state = m_browserHandle ? PlaybackState::Playing : PlaybackState::Failed;
    m_lastError = m_browserHandle ? "" : "No Web video rendition for " + path;
    return m_browserHandle != 0;
}
void VideoPlayer::Close() {
    if (m_browserHandle) ControlVideo(m_browserHandle, "close");
    m_browserHandle = 0;
    m_texture.Reset();
    m_width = m_height = 0;
    m_clock = 0;
    m_state = PlaybackState::Idle;
    m_lifecycleSuspended = m_resumeAfterLifecycle = false;
}
bool VideoPlayer::Pause() {
    if (m_state != PlaybackState::Playing) return false;
    ControlVideo(m_browserHandle, "pause");
    m_state = PlaybackState::Paused;
    return true;
}
bool VideoPlayer::Resume() {
    if (m_state != PlaybackState::Paused) return false;
    ControlVideo(m_browserHandle, "play");
    m_state = PlaybackState::Playing;
    return true;
}
void VideoPlayer::Stop() {
    if (m_browserHandle) ControlVideo(m_browserHandle, "pause");
    m_state = PlaybackState::Stopped;
}
void VideoPlayer::Skip() {
    if (m_browserHandle) ControlVideo(m_browserHandle, "pause");
    m_state = PlaybackState::Finished;
}
void VideoPlayer::SetAllSuspended(const bool suspended) {
    for (auto* player : players) {
        if (player->m_lifecycleSuspended == suspended) continue;
        player->m_lifecycleSuspended = suspended;
        if (suspended) {
            player->m_resumeAfterLifecycle = player->m_state == PlaybackState::Playing;
            if (player->m_resumeAfterLifecycle) (void)player->Pause();
        } else {
            if (player->m_resumeAfterLifecycle) (void)player->Resume();
            player->m_resumeAfterLifecycle = false;
        }
    }
}
void VideoPlayer::Update(float) {
    if (!m_browserHandle || !Playing()) return;
    if (VideoValue(m_browserHandle, "failed")) {
        char* error = VideoError(m_browserHandle);
        m_lastError = error ? error : "Web video failed";
        std::free(error);
        m_state = PlaybackState::Failed;
        return;
    }
    if (VideoValue(m_browserHandle, "ended")) { m_state = PlaybackState::Finished; return; }
    m_clock = VideoValue(m_browserHandle, "time");
    const auto width = static_cast<int>(VideoValue(m_browserHandle, "width"));
    const auto height = static_cast<int>(VideoValue(m_browserHandle, "height"));
    if (width <= 0 || height <= 0 || !VideoValue(m_browserHandle, "ready")) return;
    if (width != m_width || height != m_height) {
        m_texture.Reset();
        SDL_Texture* texture = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_ABGR8888,
            SDL_TEXTUREACCESS_STREAMING, width, height);
        if (!texture) { m_state = PlaybackState::Failed; m_lastError = SDL_GetError(); return; }
        m_texture = graphics::TextureResource::Adopt(
            graphics::TextureBackend::SdlRenderer, texture, width, height);
        m_width = width; m_height = height;
    }
#if defined(PRISMATIX_WEB_WEBGPU)
    webGpuFrame.resize(static_cast<std::size_t>(width) * height * 4);
    ReadVideo(m_browserHandle, webGpuFrame.data(), width, height);
    SDL_UpdateTexture(static_cast<SDL_Texture*>(
        m_texture.Native(graphics::TextureBackend::SdlRenderer)), nullptr, webGpuFrame.data(), width * 4);
#else
    SDL_FlushRenderer(m_renderer);
    const auto texture = static_cast<int>(SDL_GetNumberProperty(
        SDL_GetTextureProperties(static_cast<SDL_Texture*>(
            m_texture.Native(graphics::TextureBackend::SdlRenderer))),
        SDL_PROP_TEXTURE_OPENGLES2_TEXTURE_NUMBER, 0));
    if (texture) UploadVideo(m_browserHandle, texture);
#endif
}
void VideoPlayer::Render(const int width, const int height) {
    if (!m_texture || m_width <= 0 || m_height <= 0) return;
    const float scale = std::min(static_cast<float>(width) / m_width,
                                 static_cast<float>(height) / m_height);
    const SDL_FRect destination{(width - m_width * scale) / 2,
        (height - m_height * scale) / 2, m_width * scale, m_height * scale};
    SDL_RenderTexture(m_renderer, static_cast<SDL_Texture*>(
        m_texture.Native(graphics::TextureBackend::SdlRenderer)), nullptr, &destination);
}
std::string VideoPlayer::LastError() const { return m_lastError; }
QueueMetrics VideoPlayer::Queues() const { return {}; }
} // namespace px::video
#endif
