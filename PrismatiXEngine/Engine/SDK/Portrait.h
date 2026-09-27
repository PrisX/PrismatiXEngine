#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::sdk {

struct PortraitLayer {
    enum class Role : std::uint8_t { Legacy, Base, Expression };
    std::string path;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    std::uint8_t opacity = 255;
    Role role = Role::Legacy;
};

struct LayeredPortrait {
    int schemaRevision = 1;
    float width = 0.0f;
    float height = 0.0f;
    std::vector<PortraitLayer> layers;
};

// Parses a validated .pxportrait into the same layer layout used by Stage and UI.
// Invalid documents fail as a whole, so neither host can display a partial face.
[[nodiscard]] std::optional<LayeredPortrait> ParseLayeredPortrait(
    std::string_view source);

}  // namespace px::sdk
