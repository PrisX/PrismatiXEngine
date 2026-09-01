#include "Engine/Graphics/Texture.h"

#include <SDL3/SDL.h>

#include <limits>
#include <utility>

namespace px::graphics {

TextureResource::~TextureResource() { Reset(); }

TextureResource::TextureResource(TextureResource&& other) noexcept {
    *this = std::move(other);
}

TextureResource& TextureResource::operator=(TextureResource&& other) noexcept {
    if (this == &other) return *this;
    Reset();
    m_backend = std::exchange(other.m_backend, TextureBackend::None);
    m_native = std::exchange(other.m_native, nullptr);
    m_width = std::exchange(other.m_width, 0);
    m_height = std::exchange(other.m_height, 0);
    m_generation = std::exchange(other.m_generation, 0);
    return *this;
}

TextureResource TextureResource::Adopt(const TextureBackend backend,
                                       void* native, const int width,
                                       const int height,
                                       const std::uint64_t generation) {
    TextureResource result;
    if (!native || backend == TextureBackend::None || width <= 0 || height <= 0) {
        // Adopt is an ownership transfer even when metadata validation fails.
        // Do not strand a successfully-created backend resource on that path.
        if (native && backend == TextureBackend::SdlRenderer)
            SDL_DestroyTexture(static_cast<SDL_Texture*>(native));
        return result;
    }
    result.m_backend = backend;
    result.m_native = native;
    result.m_width = width;
    result.m_height = height;
    result.m_generation = generation;
    return result;
}

TextureResource TextureResource::CreateStreaming(
    const TextureBackend backend, void* device,
    const StreamingTextureFormat format, const int width, const int height,
    const std::uint64_t generation) {
    if (backend != TextureBackend::SdlRenderer || !device || width <= 0 ||
        height <= 0)
        return {};
    const SDL_PixelFormat pixelFormat =
        format == StreamingTextureFormat::Rgb24 ? SDL_PIXELFORMAT_RGB24
                                                : SDL_PIXELFORMAT_RGBA32;
    SDL_Texture* texture = SDL_CreateTexture(
        static_cast<SDL_Renderer*>(device), pixelFormat,
        SDL_TEXTUREACCESS_STREAMING, width, height);
    return Adopt(backend, texture, width, height, generation);
}

bool TextureResource::Update(const void* pixels,
                             const std::size_t pitchBytes) {
    if (!pixels || pitchBytes == 0 ||
        pitchBytes > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        m_backend != TextureBackend::SdlRenderer || !m_native)
        return false;
    return SDL_UpdateTexture(static_cast<SDL_Texture*>(m_native), nullptr,
                             pixels, static_cast<int>(pitchBytes));
}

bool TextureResource::SetLinearSampling(const bool linear) {
    if (m_backend != TextureBackend::SdlRenderer || !m_native) return false;
    return SDL_SetTextureScaleMode(
        static_cast<SDL_Texture*>(m_native),
        linear ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
}

bool TextureResource::SetAlphaBlend(const bool enabled) {
    if (m_backend != TextureBackend::SdlRenderer || !m_native) return false;
    return SDL_SetTextureBlendMode(static_cast<SDL_Texture*>(m_native),
                                   enabled ? SDL_BLENDMODE_BLEND
                                           : SDL_BLENDMODE_NONE);
}

void TextureResource::Reset() {
    if (m_backend == TextureBackend::SdlRenderer && m_native)
        SDL_DestroyTexture(static_cast<SDL_Texture*>(m_native));
    m_backend = TextureBackend::None;
    m_native = nullptr;
    m_width = 0;
    m_height = 0;
    m_generation = 0;
}

}  // namespace px::graphics
