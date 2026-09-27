#pragma once

#include "Engine/Core/Result.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace px::progress {

// Packaged games use a stable per-user directory, independent of the install
// path and package fingerprint. PRISMATIX_PLAYER_DATA_ROOT may select an
// absolute parent directory for portable deployments and isolated tests.
[[nodiscard]] Result<std::filesystem::path> ResolvePackagedUserDataDirectory(
    std::string_view gameId);

// Empty means no optional pack is enabled. One pack may be enabled at a time.
[[nodiscard]] Result<std::string> ReadEnabledOptionalPackId(
    const std::filesystem::path& userDataDirectory);
[[nodiscard]] Result<bool> WriteEnabledOptionalPackId(
    const std::filesystem::path& userDataDirectory,
    std::string_view optionalPackId);

// One-time, non-destructive import of saves written beside older Player
// executables. Existing destination files and all source files are preserved.
[[nodiscard]] Result<std::size_t> ImportLegacyPortableSaves(
    const std::filesystem::path& packageRoot,
    const std::filesystem::path& userDataDirectory);

}  // namespace px::progress
