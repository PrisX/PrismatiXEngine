#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::sdk {

// An image drawn over an expression layer while the eyes blink or the mouth
// moves, in the portrait's canvas coordinates (x/y default to the layer's).
struct PortraitFrame {
    std::string path;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct PortraitLayer {
    enum class Role : std::uint8_t { Base, Expression };
    std::string path;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    std::uint8_t opacity = 255;
    Role role = Role::Base;
    // Expression layers only. Blink frames play in order and back, from
    // half-closed to closed; mouth frames go from slightly to fully open.
    std::vector<PortraitFrame> blink;
    std::vector<PortraitFrame> mouth;
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
