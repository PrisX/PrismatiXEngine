#pragma once

#include <cmath>

// 公式我都從 https://easings.net/zh-tw 偷來的

namespace PrismatiX::Utils {
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

inline float SmoothStep(float t) { return t * t * (3.0f - 2.0f * t); }

inline float Lerp(float from, float to, float t) { return from + (to - from) * t; }

// 我為了寫這個跑去先理解指數衰減在幹嘛，還好不算太難，就大概下面這個公式
// current = current + (target - current) * factor
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

}  // namespace PrismatiX::Utils
