#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace px::graphics {

struct CustomEffectUniformDescriptor {
    std::string name;
    std::string type;
    std::uint32_t slot = 0;
    std::array<float, 4> defaultValue{};
    float minimum = 0.0f;
    float maximum = 1.0f;
};

struct CustomEffectArtifactDescriptor {
    std::string format;
    std::string asset;
    std::string fingerprint;
};

struct CustomEffectDescriptor {
    std::string id;
    std::uint32_t schemaRevision = 2;
    // stage: whole Stage post-processing; node: one Stage graph leaf;
    // transition: outgoing/incoming screen textures.
    std::string targetLayer;
    std::vector<CustomEffectUniformDescriptor> uniforms;
    std::vector<CustomEffectArtifactDescriptor> artifacts;
    std::uint32_t samplerCount = 0;
    std::uint32_t uniformBufferCount = 0;
};

inline bool IsCustomEffectTarget(const std::string_view value) {
    return value == "stage" || value == "node" || value == "transition";
}

// Author-facing values resolved through the .pxeffect uniform schema.  The
// renderer owns slot/type/range validation so every caller uses the same
// contract and scripts never receive raw GPU resources.
using CustomEffectNamedParameters =
    std::map<std::string, std::vector<float>, std::less<>>;

}  // namespace px::graphics
