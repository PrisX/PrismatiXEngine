#include "Engine/Graphics/BuiltInShaders.h"
#include "Engine/Graphics/WebBackend.h"

#if defined(PRISMATIX_WEB_WEBGPU)
#include <SDL3/SDL_gpu.h>

#include <cstring>

namespace px::graphics {

// Embedded by PrismatiXEngine/CMakeLists.txt from Shaders/Builtin/webgpu.
extern const char kBuiltInCompositorWgsl[];
extern const char kBuiltInRuleWgsl[];

namespace {
std::optional<ShaderBytecodeView> SelectWgsl(const char* source,
                                             const std::uint32_t supportedFormats) {
    if ((supportedFormats & SDL_GPU_SHADERFORMAT_WGSL) == 0) return std::nullopt;
    return ShaderBytecodeView{reinterpret_cast<const std::uint8_t*>(source),
                              std::strlen(source), SDL_GPU_SHADERFORMAT_WGSL, "main"};
}
}  // namespace

std::optional<ShaderBytecodeView> SelectBuiltInCompositorShader(
    const std::uint32_t supportedFormats) {
    return SelectWgsl(kBuiltInCompositorWgsl, supportedFormats);
}

std::optional<ShaderBytecodeView> SelectBuiltInRuleShader(
    const std::uint32_t supportedFormats) {
    return SelectWgsl(kBuiltInRuleWgsl, supportedFormats);
}

}  // namespace px::graphics
#elif defined(__EMSCRIPTEN__)
namespace px::graphics {

std::optional<ShaderBytecodeView> SelectBuiltInCompositorShader(
    const std::uint32_t) {
    return std::nullopt;
}

}  // namespace px::graphics
#endif
