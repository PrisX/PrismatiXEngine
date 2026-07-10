#pragma once

#include <cmath>
#include <string_view>

// 公式我都從 https://easings.net/zh-tw 偷來的

namespace px::support {

namespace detail {
constexpr float kPi = 3.14159265358979323846f;
}

inline float Linear(float t) { return t; }

inline float EaseInQuad(float t) { return t * t; }

inline float EaseOutQuad(float t) { return t * (2.0f - t); }

inline float EaseInOutQuad(float t) { return (t < 0.5f) ? (2.0f * t * t) : (-1.0f + (4.0f - 2.0f * t) * t); }

inline float EaseOutCubic(float t) {
    float f = t - 1.0f;
    return f * f * f + 1.0f;
}

inline float EaseInOutCubic(float t) { return (t < 0.5f) ? (4.0f * t * t * t) : ((t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f); }

inline float EaseOutBack(float t) {
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float f = t - 1.0f;
    return 1.0f + c3 * f * f * f + c1 * f * f;
}

inline float EaseInCubic(float t) { return t * t * t; }

inline float EaseInQuart(float t) { return t * t * t * t; }
inline float EaseOutQuart(float t) {
    const float f = t - 1.0f;
    return 1.0f - f * f * f * f;
}
inline float EaseInOutQuart(float t) {
    return t < 0.5f ? 8.0f * t * t * t * t
                    : 1.0f - 8.0f * (t - 1.0f) * (t - 1.0f) * (t - 1.0f) * (t - 1.0f);
}

inline float EaseInSine(float t) { return 1.0f - std::cos(t * detail::kPi * 0.5f); }
inline float EaseOutSine(float t) { return std::sin(t * detail::kPi * 0.5f); }
inline float EaseInOutSine(float t) { return -(std::cos(detail::kPi * t) - 1.0f) * 0.5f; }

inline float EaseInExpo(float t) { return t <= 0.0f ? 0.0f : std::pow(2.0f, 10.0f * t - 10.0f); }
inline float EaseOutExpo(float t) { return t >= 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t); }
inline float EaseInOutExpo(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t < 0.5f ? std::pow(2.0f, 20.0f * t - 10.0f) * 0.5f
                    : (2.0f - std::pow(2.0f, -20.0f * t + 10.0f)) * 0.5f;
}

inline float EaseInBack(float t) {
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    return c3 * t * t * t - c1 * t * t;
}
inline float EaseInOutBack(float t) {
    const float c1 = 1.70158f;
    const float c2 = c1 * 1.525f;
    return t < 0.5f
               ? ((2.0f * t) * (2.0f * t) * ((c2 + 1.0f) * 2.0f * t - c2)) * 0.5f
               : ((2.0f * t - 2.0f) * (2.0f * t - 2.0f) * ((c2 + 1.0f) * (2.0f * t - 2.0f) + c2) +
                  2.0f) *
                     0.5f;
}

inline float EaseInElastic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const float c4 = 2.0f * detail::kPi / 3.0f;
    return -std::pow(2.0f, 10.0f * t - 10.0f) * std::sin((t * 10.0f - 10.75f) * c4);
}
inline float EaseOutElastic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const float c4 = 2.0f * detail::kPi / 3.0f;
    return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
}

inline float EaseOutBounce(float t) {
    const float n1 = 7.5625f;
    const float d1 = 2.75f;
    if (t < 1.0f / d1) return n1 * t * t;
    if (t < 2.0f / d1) {
        t -= 1.5f / d1;
        return n1 * t * t + 0.75f;
    }
    if (t < 2.5f / d1) {
        t -= 2.25f / d1;
        return n1 * t * t + 0.9375f;
    }
    t -= 2.625f / d1;
    return n1 * t * t + 0.984375f;
}
inline float EaseInBounce(float t) { return 1.0f - EaseOutBounce(1.0f - t); }

inline float SmoothStep(float t) { return t * t * (3.0f - 2.0f * t); }

// Canonical easing names used by [anim ease=..], the editor node options, and
// Engine.Animate in Lua. Unknown names fall back to linear.
inline float Ease(std::string_view name, float t) {
    if (t <= 0.0f) return name == "outBounce" || name == "inBounce" ? 0.0f : 0.0f;
    if (t >= 1.0f) return 1.0f;
    if (name == "linear") return Linear(t);
    if (name == "inQuad") return EaseInQuad(t);
    if (name == "outQuad") return EaseOutQuad(t);
    if (name == "inOutQuad") return EaseInOutQuad(t);
    if (name == "inCubic") return EaseInCubic(t);
    if (name == "outCubic") return EaseOutCubic(t);
    if (name == "inOutCubic") return EaseInOutCubic(t);
    if (name == "inQuart") return EaseInQuart(t);
    if (name == "outQuart") return EaseOutQuart(t);
    if (name == "inOutQuart") return EaseInOutQuart(t);
    if (name == "inSine") return EaseInSine(t);
    if (name == "outSine") return EaseOutSine(t);
    if (name == "inOutSine") return EaseInOutSine(t);
    if (name == "inExpo") return EaseInExpo(t);
    if (name == "outExpo") return EaseOutExpo(t);
    if (name == "inOutExpo") return EaseInOutExpo(t);
    if (name == "inBack") return EaseInBack(t);
    if (name == "outBack") return EaseOutBack(t);
    if (name == "inOutBack") return EaseInOutBack(t);
    if (name == "inElastic") return EaseInElastic(t);
    if (name == "outElastic") return EaseOutElastic(t);
    if (name == "inBounce") return EaseInBounce(t);
    if (name == "outBounce") return EaseOutBounce(t);
    if (name == "smoothstep") return SmoothStep(t);
    return Linear(t);
}

inline float Lerp(float from, float to, float t) { return from + (to - from) * t; }

// 我為了寫這個跑去先理解指數衰減在幹嘛，還好不算太難，就大概下面這個公式
inline bool ExpDecay(float& current, float target, float factor, float snapThresholdSq = 0.25f) {
    float dx = target - current;
    current += dx * factor;
    // 因為 dx 不一定是正的，所以先平方
    // 啊 snapThresholdSq 就是距離的平方 (square)，因為我原本如果直接寫 dx < 0.5 的話 dx 如果是負的就爛了

    if (dx * dx < snapThresholdSq) {
        // 就是夠近就直接等於過去，雖然因為 float 精度的關係好像是會到達
        current = target;
        return true;
    }

    return false;
}

}
