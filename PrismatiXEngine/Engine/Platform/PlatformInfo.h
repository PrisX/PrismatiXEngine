#pragma once

#include <string_view>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace px {

// The platform the Player was built for. Mobile platforms have no window to
// size or restore: the game always fills the screen.
struct PlatformInfo {
    std::string_view os;
    bool mobile = false;
};

[[nodiscard]] constexpr PlatformInfo CurrentPlatform() {
#if defined(__ANDROID__)
    return {"android", true};
#elif defined(__APPLE__)
#if TARGET_OS_IPHONE
    return {"ios", true};
#else
    return {"macos", false};
#endif
#elif defined(_WIN32)
    return {"windows", false};
#elif defined(__EMSCRIPTEN__)
    return {"web", false};
#else
    return {"linux", false};
#endif
}

}  // namespace px
