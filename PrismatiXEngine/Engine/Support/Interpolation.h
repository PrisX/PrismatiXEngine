#pragma once

#include "Engine/Core/Variant.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace px::support {

enum class IntegerInterpolation { Discrete, Rounded };

// Canonical interpolation for Runtime Variant values. Callers retain control
// over whether integer keys are stepped (UI state) or rounded (timeline data).
// Non-numeric values intentionally remain discrete.
inline std::optional<Variant> InterpolateVariant(
    const Variant& from, const Variant& to, const float amount,
    const IntegerInterpolation integers = IntegerInterpolation::Discrete) {
    if (from.Type() != to.Type()) return std::nullopt;
    const float t = std::clamp(amount, 0.0f, 1.0f);
    switch (from.Type()) {
        case VariantType::Integer: {
            if (integers == IntegerInterpolation::Discrete)
                return t < 1.0f ? from.Clone() : to.Clone();
            const auto a = *from.TryGet<std::int64_t>();
            const auto b = *to.TryGet<std::int64_t>();
            return Variant(static_cast<std::int64_t>(std::llround(
                static_cast<double>(a) +
                (static_cast<double>(b) - static_cast<double>(a)) * t)));
        }
        case VariantType::Number: {
            const double a = *from.TryGet<double>();
            const double b = *to.TryGet<double>();
            return Variant(a + (b - a) * static_cast<double>(t));
        }
        case VariantType::Vec2: {
            const Vec2 a = *from.TryGet<Vec2>();
            const Vec2 b = *to.TryGet<Vec2>();
            return Variant(Vec2{a.x + (b.x - a.x) * t,
                                a.y + (b.y - a.y) * t});
        }
        case VariantType::Rect: {
            const Rect a = *from.TryGet<Rect>();
            const Rect b = *to.TryGet<Rect>();
            return Variant(Rect{a.x + (b.x - a.x) * t,
                                a.y + (b.y - a.y) * t,
                                a.w + (b.w - a.w) * t,
                                a.h + (b.h - a.h) * t});
        }
        case VariantType::Color: {
            const Color a = *from.TryGet<Color>();
            const Color b = *to.TryGet<Color>();
            const auto channel = [t](const std::uint8_t x,
                                     const std::uint8_t y) {
                return static_cast<std::uint8_t>(std::clamp(
                    std::lround(static_cast<float>(x) +
                                (static_cast<float>(y) - x) * t),
                    0l, 255l));
            };
            return Variant(Color{channel(a.r, b.r), channel(a.g, b.g),
                                 channel(a.b, b.b), channel(a.a, b.a)});
        }
        default: return t < 1.0f ? from.Clone() : to.Clone();
    }
}

}  // namespace px::support
