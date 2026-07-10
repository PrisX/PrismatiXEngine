#pragma once

#include <compare>
#include <cstdint>

namespace px {

struct Color {
    std::uint8_t r = 255;
    std::uint8_t g = 255;
    std::uint8_t b = 255;
    std::uint8_t a = 255;

    auto operator<=>(const Color&) const = default;
};

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    auto operator<=>(const Vec2&) const = default;
};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    auto operator<=>(const Rect&) const = default;

    [[nodiscard]] bool Contains(float px, float py) const {
        return px >= x && px <= x + w && py >= y && py <= y + h;
    }
};

}
