#pragma once

#include "Engine/Core/Result.h"

#include <string>

namespace px::ui {

// Registers the production UI controls and exports their authoring metadata
// through the versioned SDK contract. Preview and Player query the same
// TypeRegistry at runtime.
[[nodiscard]] Result<std::string> BuildUiTypeRegistryManifest();

}  // namespace px::ui
