#pragma once

#include "Engine/SDK/Packager.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
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
inline constexpr std::string_view kPackageOverlaySignatureUri =
    "__prismatix_overlay__/signature.bin";

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
    const PackageVerificationProgress& onProgress = {},
    std::stop_token stop = {});

struct PackageOverlayVerification {
    bool valid = false;
    std::string error;
    std::string targetManifestJson;
    std::string targetContentIndexJson;
    std::string optionalPackId;
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

// Requires a pinned public key and a valid revision-2 or revision-3 publisher signature.
// Player must use this entry point before an automatic overlay mount.
[[nodiscard]] PackageOverlayVerification VerifySignedPackageOverlay(
    const std::filesystem::path& distribution,
    const std::filesystem::path& overlayPath);

// After a prior signed overlay has been verified, its target documents may be
// used as the exact base for the next signed overlay in an ordered chain.
[[nodiscard]] PackageOverlayVerification VerifySignedPackageOverlayFromDocuments(
    std::string_view baseManifestJson,
    std::string_view baseContentIndexJson,
    const std::filesystem::path& overlayPath);

struct PackageOverlayBuildResult {
    bool created = false;
    bool planned = false;
    std::string error;
    std::uint64_t changedResources = 0;
    std::uint64_t removedResources = 0;
    std::uint64_t changedRawBytes = 0;
    std::vector<sdk::PackageDiagnostic> diagnostics;
};

struct PackageOverlayChainMount {
    std::filesystem::path path;
    std::vector<std::string> tombstones;
};

struct PackageOverlayChain {
    bool valid = false;
    std::string error;
    std::string targetManifestJson;
    std::string targetContentIndexJson;
    std::vector<PackageOverlayChainMount> mounts;
    std::string optionalPackId;
    std::string mainPackageFingerprint;
    std::vector<std::string> compatibleSaveFingerprints;
    // Taken before an optional pack is mounted; optional content cannot
    // change which old saves or persistence keys the main release accepts.
    std::vector<std::string> mainCompatibleSaveFingerprints;
    std::vector<std::string> mainLegacyPersistenceSecrets;
    std::vector<std::string> ignoredFiles;
};

// Scans a dedicated updates directory for bounded regular PDX candidates,
// then finds the unique signed exact-base chain. An absent directory means no
// update. Ambiguous branches fail closed; stale/unrelated files are ignored.
// An enabled revision-3 optional pack mounts last only when it verifies
// against the final main update. Empty ID leaves optional packs disabled.
// A verifier can exclude the candidate it is checking when that file is
// already present in the same updates directory.
[[nodiscard]] PackageOverlayChain ResolveSignedPackageOverlayChain(
    const std::filesystem::path& baseDistribution,
    const std::filesystem::path& updatesDirectory,
    std::string_view enabledOptionalPackId = {},
    const std::filesystem::path& excludedOverlayPath = {});

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
    const PackageOverlayBuildProgress& onProgress = {},
    const std::filesystem::path& signingKeyFile = {},
    std::string_view optionalPackId = {});

// Creates a content-only overlay directly from a fully validated artifact
// plan. The caller must plan with AllInputs so unchanged artifact sources are
// checked before publication; changed file bytes are checked again by the
// streaming writer. The base runtime must match playerExecutable exactly.
// priorUpdatesDirectory, when supplied, must contain the full unique signed
// main update chain; its final target documents become the exact patch base.
// planOnly performs full input/base preflight and returns a changed resource
// inventory without creating a staging directory or output PDX.
[[nodiscard]] PackageOverlayBuildResult CreatePackageOverlayFromPlan(
    const std::filesystem::path& baseDistribution,
    const std::filesystem::path& artifactRoot,
    const sdk::PackagePlanResult& plan,
    const std::filesystem::path& playerExecutable,
    const std::filesystem::path& outputPath,
    const PackageOverlayBuildProgress& onProgress = {},
    const std::filesystem::path& signingKeyFile = {},
    std::string_view optionalPackId = {},
    const std::filesystem::path& priorUpdatesDirectory = {},
    bool planOnly = false);

// Plans a fully verified artifact and verifies the independent base PDX shards
// at the same time. Both checks must finish before an overlay can be written;
// the base metadata and archive file stamps are pinned through publication.
[[nodiscard]] PackageOverlayBuildResult CreatePackageOverlayFromRequest(
    const std::filesystem::path& baseDistribution,
    const sdk::PackageRequest& request,
    const std::filesystem::path& outputPath,
    const sdk::PackageEventSink& onPackageEvent = {},
    const PackageOverlayBuildProgress& onProgress = {},
    const std::filesystem::path& signingKeyFile = {},
    std::string_view optionalPackId = {},
    const std::filesystem::path& priorUpdatesDirectory = {},
    std::string_view previousManifestJson = {},
    bool planOnly = false);

}  // namespace px::io
