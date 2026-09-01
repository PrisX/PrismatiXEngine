#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace px::graphics {

struct ShaderBytecodeView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::uint32_t format = 0;
    const char* entrypoint = nullptr;
};

// Returns an offline-compiled built-in compositor shader in a format accepted
// by the active device. No source compiler is linked into the Player.
[[nodiscard]] std::optional<ShaderBytecodeView>
SelectBuiltInCompositorShader(std::uint32_t supportedFormats);

}  // namespace px::graphics
