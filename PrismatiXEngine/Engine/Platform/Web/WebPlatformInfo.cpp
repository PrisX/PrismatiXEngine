#if defined(PRISMATIX_WEB_PLAYER)
#include "Engine/Platform/PlatformInfo.h"
#include <emscripten/emscripten.h>
namespace px {
EM_JS(int, WebIsMobile, (), {
    return navigator.userAgentData?.mobile || /Android|iPhone|iPad|iPod/i.test(navigator.userAgent) ||
        (navigator.platform === "MacIntel" && navigator.maxTouchPoints > 1) ? 1 : 0;
});
PlatformInfo CurrentPlatform() { return {"web", WebIsMobile() != 0}; }
}
#endif
