#pragma once

#include "Engine/Accessibility/SemanticTree.h"

#include <memory>
#include <string_view>

struct SDL_Window;

namespace px::accessibility {

// Creates the native accessibility bridge for the SDL window. A null result
// is a startup failure for the shipped desktop Player, not a silent fallback.
[[nodiscard]] std::shared_ptr<SemanticAdapter> CreatePlatformSemanticAdapter(
    SDL_Window* window);
[[nodiscard]] std::string_view PlatformAccessibilityBackend();

}  // namespace px::accessibility
