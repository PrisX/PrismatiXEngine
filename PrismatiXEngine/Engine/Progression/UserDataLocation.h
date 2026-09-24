#pragma once

#include "Engine/Core/Result.h"

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace px::progress {

// Packaged games use a stable per-user directory, independent of the install
// path and package fingerprint. PRISMATIX_PLAYER_DATA_ROOT may select an
// absolute parent directory for portable deployments and isolated tests.
[[nodiscard]] Result<std::filesystem::path> ResolvePackagedUserDataDirectory(
    std::string_view gameId);

// One-time, non-destructive import of saves written beside older Player
// executables. Existing destination files and all source files are preserved.
[[nodiscard]] Result<std::size_t> ImportLegacyPortableSaves(
    const std::filesystem::path& packageRoot,
    const std::filesystem::path& userDataDirectory);

}  // namespace px::progress
