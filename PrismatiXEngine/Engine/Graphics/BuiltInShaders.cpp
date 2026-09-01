#include "Engine/Graphics/BuiltInShaders.h"

#if defined(__EMSCRIPTEN__)
namespace px::graphics {

std::optional<ShaderBytecodeView> SelectBuiltInCompositorShader(
    const std::uint32_t) {
    return std::nullopt;
}

}  // namespace px::graphics
#endif
