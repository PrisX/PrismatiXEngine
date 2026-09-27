#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace px::io {

inline constexpr std::string_view kPackageOverlayPrefix =
    "__prismatix_overlay__/";
inline constexpr std::string_view kPackageOverlayManifestUri =
    "__prismatix_overlay__/manifest.json";
inline constexpr std::string_view kPackageOverlayTargetManifestUri =
    "__prismatix_overlay__/target-manifest.json";
inline constexpr std::string_view kPackageOverlayTargetIndexUri =
    "__prismatix_overlay__/target-content-index.json";

struct PackageContentVerification {
    bool valid = false;
    std::string error;
    std::uint64_t resources = 0;
    std::uint64_t rawBytes = 0;
    std::uint64_t archives = 0;
};

using PackageVerificationProgress =
    std::function<void(std::uint64_t current, std::uint64_t total,
                       std::uint64_t rawBytes)>;

// Checks the packaged resource inventory against every mounted PDX entry.
// This is an integrity check, not signature/authenticity verification.
[[nodiscard]] PackageContentVerification VerifyPackageContents(
    const std::filesystem::path& distribution,
    const PackageVerificationProgress& onProgress = {});

struct PackageOverlayVerification {
    bool valid = false;
    std::string error;
    std::string targetManifestJson;
    std::vector<std::string> tombstones;
    std::uint64_t changedResources = 0;
    std::uint64_t changedRawBytes = 0;
};

// Verifies an unsigned, exact-base-metadata PDX overlay before its entries or
// target manifest are used. Checks changed payload bytes, but does not rehash
// all base PDX data or establish publisher authenticity; callers must decide
// whether to trust the patch source.
[[nodiscard]] PackageOverlayVerification VerifyPackageOverlay(
    const std::filesystem::path& distribution,
    const std::filesystem::path& overlayPath);

struct PackageOverlayBuildResult {
    bool created = false;
    std::string error;
    std::uint64_t changedResources = 0;
    std::uint64_t removedResources = 0;
    std::uint64_t changedRawBytes = 0;
};

using PackageOverlayBuildProgress = std::function<void(
    std::string_view phase, std::uint64_t current, std::uint64_t total,
    std::uint64_t rawBytes)>;

// Builds one content-only overlay from two complete distributions. Both are
// fully verified first, and non-content runtime files must match exactly.
// The output must be a new absolute .pdx path outside either distribution.
[[nodiscard]] PackageOverlayBuildResult CreatePackageOverlay(
    const std::filesystem::path& baseDistribution,
    const std::filesystem::path& targetDistribution,
    const std::filesystem::path& outputPath,
    const PackageOverlayBuildProgress& onProgress = {});

}  // namespace px::io
