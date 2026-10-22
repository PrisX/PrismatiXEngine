#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::sdk {

enum class PackageCompression {
    None,
    Fast,
    Balanced,
    Maximum,
};

enum class PackageValidationDepth {
    Structural,
    AllInputs,
};

struct PackageInput {
    std::string uri;
    std::string fingerprint;
    std::uint64_t size = 0;
};

struct PackageRoute {
    std::string id;
    std::string scene;
};

struct PackageSaveMigration {
    std::string id;
    std::string fromContentVersion;
    std::uint32_t fromSaveVersion = 0;
    std::string toContentVersion;
    std::uint32_t toSaveVersion = 0;
    std::string asset;
};

struct PackagePersistenceKey {
    std::string keyId;
    std::string secret;
};

// Save identity of the most recent release, recorded in the author project
// (.prismatix-release/lineage.json) rather than in an output directory, so a
// new output folder or a clean CI checkout keeps accepting existing saves.
struct PackageSaveLineage {
    std::string packageFingerprint;
    std::string contentVersion;
    std::uint32_t saveVersion = 0;
    std::vector<std::string> compatibleSaveFingerprints;
    std::vector<PackagePersistenceKey> legacyReaders;
};

struct PackageRequest {
    std::string requestId;
    std::string gameId;
    std::filesystem::path projectRoot;
    std::filesystem::path outputDir;
    // Optional read-only source for reusable compressed entries when a
    // frontend publishes through its own staging directory.
    std::filesystem::path cacheSourceDir;
    // Only set when the frontend will atomically replace and retire the cache
    // source after this staged output is promoted. Prevents persistent
    // cross-distribution hard links to mutable archive files.
    bool retireCacheSourceAfterPromotion = false;
    std::filesystem::path playerExecutable;
    // Optional distribution branding: the packaged Player's file name without
    // its extension, and an .ico or square .png icon for the executable
    // (Windows), the .app (macOS, as .icns) and the .desktop entry (Linux).
    std::string executableName;
    std::filesystem::path icon;
    // Shown in Explorer's Details tab, Info.plist and the .desktop entry.
    // productName defaults to title; version is the authored version string.
    std::string productName;
    std::string companyName;
    std::string copyright;
    std::string version;
    // Windows: "flat" keeps runtime libraries and game data beside the
    // executable; "bundled" moves them into engineFolder (default "Engine").
    std::string layout = "flat";
    std::string engineFolder;
    // Apply the host platform's shipping layout: a macOS .app bundle, or a
    // Linux lib/ folder with a .desktop entry. Intermediate packs (Android,
    // Web) leave it off and get the plain layout.
    bool platformLayout = false;
    // macOS CFBundleIdentifier; derived from gameId when empty.
    std::string bundleIdentifier;
    // The desktop platform the distribution is for (win32-x64, linux-x64,
    // darwin-x64 or darwin-arm64): its layout, runtime libraries and shader
    // formats, with playerExecutable that platform's Player. Empty: the host.
    std::string targetPlatform;
    std::string title;
    int width = 1280;
    int height = 720;
    std::string startScript;
    std::string sourceMap;
    std::string startRoute;
    std::vector<PackageRoute> routes;
    std::vector<PackageSaveMigration> saveMigrations;
    std::vector<std::string> extensions;
    std::string contentVersion;
    std::uint32_t saveVersion = 1;
    std::string graphicsTier = "basic";
    bool encryption = false;
    // SEC1 uncompressed P-256 public key, lowercase hex. Optional for
    // distributions that do not enable signed content overlays.
    std::string publisherPublicKey;
    PackageCompression compression = PackageCompression::Balanced;
    // Stable URI-hash partitions. One preserves the legacy Content.pdx layout.
    std::uint32_t archiveShards = 1;
    std::vector<PackageInput> inputs;
    std::filesystem::path cancelFile;
    // Combined with the persistence of the distribution being replaced.
    std::optional<PackageSaveLineage> saveLineage;
};

struct PackageDiagnostic {
    std::string code;
    std::string message;
    bool retryable = false;
    std::string severity = "error";
    std::string documentId{};
    std::string sourceId{};
    struct SourceSpan {
        struct Position {
            std::uint32_t line = 0;
            std::uint32_t column = 0;
            std::uint64_t offset = 0;
        };
        std::string path{};
        Position start{};
        Position end{};
    };
    std::optional<SourceSpan> span{};
    std::string hint{};
    std::string cause{};
};

struct PackageRequestParseResult {
    PackageRequest request;
    std::vector<PackageDiagnostic> diagnostics;

    [[nodiscard]] bool Valid() const { return diagnostics.empty(); }
};

enum class PackageEventKind {
    Progress,
    Completed,
    Cancelled,
    Failed,
    Validated,
};

struct PackageEvent {
    PackageEventKind kind = PackageEventKind::Progress;
    std::string requestId;

    // progress
    std::string phase;
    std::uint64_t current = 0;
    std::uint64_t total = 0;
    std::string message;

    // completed
    std::filesystem::path outputDir;
    std::filesystem::path playerExecutable;
    std::filesystem::path manifestPath;
    std::uint64_t inputCount = 0;

    // failed
    std::string code;
    bool retryable = false;
    std::vector<PackageDiagnostic> diagnostics;
    // Validation-only result: media hashes intentionally left for packaging.
    std::uint64_t deferredMediaCount = 0;
};

enum class PackageExitCode : int {
    Completed = 0,
    Failed = 1,
    Cancelled = 2,
};

struct PackageRunResult {
    PackageExitCode exitCode = PackageExitCode::Failed;
    std::vector<PackageDiagnostic> diagnostics;

    [[nodiscard]] bool Completed() const { return exitCode == PackageExitCode::Completed; }
};

struct PackagePlannedResource {
    std::string uri;
    std::filesystem::path source;
    std::vector<std::uint8_t> generatedData;
    std::uint64_t size = 0;
    std::string fingerprint;
};

struct PackagePlanResult {
    bool valid = false;
    bool allInputsVerified = false;
    std::string previousManifestSha256;
    std::vector<PackageDiagnostic> diagnostics;
    std::string manifestJson;
    std::string contentIndexJson;
    std::vector<PackagePlannedResource> resources;
};

using PackageEventSink = std::function<void(const PackageEvent&)>;

// Parses the strict frontend-to-Packager request contract. Paths remain native
// filesystem paths, while input URIs are always normalized forward-slash
// project-relative runtime paths.
[[nodiscard]] PackageRequestParseResult ParsePackageRequest(std::string_view json);

// The directory of a distribution that holds Package/manifest.json and the
// archives: the distribution itself (flat), its engine folder (Windows
// bundled, recognised by its <folder>/<folder>.manifest) or
// <Name>.app/Contents/Resources (macOS). Returns `distribution` unchanged when
// no single such directory exists.
[[nodiscard]] std::filesystem::path DistributionDataRoot(const std::filesystem::path& distribution);

// Serializes one protocol event. The CLI writes each returned object as one
// stdout line and never mixes log text into stdout.
[[nodiscard]] std::string SerializePackageEvent(const PackageEvent& event);

// Returns a lowercase SHA-256 fingerprint, or an empty string when the file
// cannot be read.
[[nodiscard]] std::string ComputePackageFingerprint(const std::filesystem::path& path);

// Validates the request and creates a Player distribution with atomic output
// promotion. Cancellation is polled between work units and immediately before
// promotion.
[[nodiscard]] PackageRunResult RunPackager(const PackageRequest& request, const PackageEventSink& eventSink = {});

// Validates the same authoring inputs and computes the exact manifest, index,
// and resource inventory that a complete distribution would contain. It does
// not write archives or attest media bytes deferred until a writer consumes
// each planned source; callers must verify those fingerprints at that point.
// previousManifestJson may come only from a verified signed predecessor;
// the overlay writer pins its hash to that exact predecessor before publishing.
[[nodiscard]] PackagePlanResult PlanPackageRequest(
    const PackageRequest& request, const PackageEventSink& eventSink = {},
    PackageValidationDepth depth = PackageValidationDepth::Structural,
    std::string_view previousManifestJson = {});

// Checks native package request and authored Story/UI/resource contracts
// without creating or replacing a distribution. Media payload fingerprints
// deferred by normal packaging are also deferred here unless depth is
// AllInputs. This checks the current input bytes, not output PDX contents or a
// guarantee that sources cannot change afterward.
[[nodiscard]] PackageRunResult ValidatePackageRequest(const PackageRequest& request,
                                                      const PackageEventSink& eventSink = {},
                                                      PackageValidationDepth depth = PackageValidationDepth::Structural);

}  // namespace px::sdk
