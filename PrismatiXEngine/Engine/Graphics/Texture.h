#pragma once

#include <cstddef>
#include <cstdint>

namespace px::graphics {

enum class TextureBackend : std::uint8_t {
    None,
    SdlRenderer,
};

enum class StreamingTextureFormat : std::uint8_t {
    Rgb24,
    Rgba32,
};

// Non-owning backend-neutral texture identity. Gameplay, Stage, UI, and SDK
// code can size and pass textures without depending on SDL_Texture or a GPU
// backend ABI. Only graphics backend implementations inspect Native().
class TextureHandle {
public:
    TextureHandle() = default;
    [[nodiscard]] explicit operator bool() const {
        return m_backend != TextureBackend::None && m_native != nullptr;
    }
    [[nodiscard]] TextureBackend Backend() const { return m_backend; }
    [[nodiscard]] int Width() const { return m_width; }
    [[nodiscard]] int Height() const { return m_height; }
    [[nodiscard]] std::uint64_t Generation() const { return m_generation; }

    [[nodiscard]] void* Native(TextureBackend expected) const {
        return expected == m_backend ? m_native : nullptr;
    }

private:
    friend class TextureResource;
    TextureHandle(TextureBackend backend, void* native, int width, int height,
                  std::uint64_t generation)
        : m_backend(backend), m_native(native), m_width(width),
          m_height(height), m_generation(generation) {}

    TextureBackend m_backend = TextureBackend::None;
    void* m_native = nullptr;
    int m_width = 0;
    int m_height = 0;
    std::uint64_t m_generation = 0;
};

// Move-only owner for backend resources. Creation takes opaque backend device
// and texture pointers so the public header remains SDL/GPU independent.
class TextureResource {
public:
    TextureResource() = default;
    ~TextureResource();
    TextureResource(const TextureResource&) = delete;
    TextureResource& operator=(const TextureResource&) = delete;
    TextureResource(TextureResource&& other) noexcept;
    TextureResource& operator=(TextureResource&& other) noexcept;

    [[nodiscard]] static TextureResource Adopt(
        TextureBackend backend, void* native, int width, int height,
        std::uint64_t generation = 0);
    [[nodiscard]] static TextureResource CreateStreaming(
        TextureBackend backend, void* device, StreamingTextureFormat format,
        int width, int height, std::uint64_t generation = 0);

    [[nodiscard]] explicit operator bool() const {
        return static_cast<bool>(Handle());
    }
    [[nodiscard]] TextureHandle Handle() const {
        return TextureHandle(m_backend, m_native, m_width, m_height,
                             m_generation);
    }
    [[nodiscard]] void* Native(TextureBackend expected) const {
        return Handle().Native(expected);
    }

    bool Update(const void* pixels, std::size_t pitchBytes);
    bool SetLinearSampling(bool linear = true);
    bool SetAlphaBlend(bool enabled = true);
    void Reset();

private:
    TextureBackend m_backend = TextureBackend::None;
    void* m_native = nullptr;
    int m_width = 0;
    int m_height = 0;
    std::uint64_t m_generation = 0;
};

}  // namespace px::graphics
