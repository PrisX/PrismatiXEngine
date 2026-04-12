#pragma once

namespace PrismatiX::App {

template <typename T = int>
inline T LInt(float val) {
    return static_cast<T>(val);
}

}  // namespace PrismatiX::App
