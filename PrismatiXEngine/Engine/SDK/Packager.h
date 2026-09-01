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

struct PackageRequest {
    std::string requestId;
    std::string gameId;
    std::filesystem::path projectRoot;
    std::filesystem::path outputDir;
    std::filesystem::path playerExecutable;
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
    PackageCompression compression = PackageCompression::Balanced;
    std::vector<PackageInput> inputs;
    std::filesystem::path cancelFile;
};

struct PackageDiagnostic {
    std::string code;
    std::string message;
    bool retryable = false;
    std::string severity = "error";
    std::string documentId;
    std::string sourceId;
    struct SourceSpan {
        struct Position {
            std::uint32_t line = 0;
            std::uint32_t column = 0;
            std::uint64_t offset = 0;
        };
        std::string path;
        Position start;
        Position end;
    };
    std::optional<SourceSpan> span;
    std::string hint;
    std::string cause;
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

using PackageEventSink = std::function<void(const PackageEvent&)>;

// Parses the strict frontend-to-Packager request contract. Paths remain native
// filesystem paths, while input URIs are always normalized forward-slash
// project-relative runtime paths.
[[nodiscard]] PackageRequestParseResult ParsePackageRequest(std::string_view json);

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

}  // namespace px::sdk
