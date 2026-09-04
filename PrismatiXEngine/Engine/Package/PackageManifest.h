#pragma once

#include "Engine/SDK/Packager.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Runtime-private package wire contract. This header is intentionally outside
// Engine/SDK and is not installed with the Native SDK; authoring integrations
// exchange PackageRequest/PackageEvent instead of depending on Player internals.
namespace px::sdk::detail {

struct PackageArchiveMount {
    std::string file;
    std::string group;
    bool optional = false;
};

struct PackageManifestRoute {
    std::string id;
    std::string sceneId;
    std::string scene;
};

struct PackageEffectUniform {
    std::string name;
    std::string type;
    std::uint32_t slot = 0;
    std::array<float, 4> defaultValue{};
    float minimum = 0.0f;
    float maximum = 1.0f;
};

struct PackageShaderArtifact {
    std::string format;
    std::string asset;
    std::string fingerprint;
};

struct PackageCustomEffect {
    std::string id;
    std::uint32_t schemaRevision = 2;
    std::string targetLayer;
    std::vector<PackageEffectUniform> uniforms;
    std::vector<PackageShaderArtifact> artifacts;
    std::uint32_t samplerCount = 0;
    std::uint32_t uniformBufferCount = 0;
};

struct PackageManifest {
    std::string engineVersion;
    std::string gameId;
    std::string title;
    int width = 1280;
    int height = 720;
    std::string startRuntimeIr;
    std::string sourceMap;
    std::string startRoute;
    std::vector<PackageManifestRoute> routes;
    std::vector<PackageArchiveMount> archives;
    std::vector<PackageSaveMigration> saveMigrations;
    std::vector<std::string> extensions;
    std::vector<PackageCustomEffect> customEffects;
    std::string contentVersion;
    std::uint32_t saveVersion = 1;
    std::string packageFingerprint;
    bool encrypted = false;
    std::string archiveKey;
    std::string graphicsTier = "basic";
};

struct PackageManifestParseResult {
    PackageManifest manifest;
    std::vector<PackageDiagnostic> diagnostics;

    [[nodiscard]] bool Valid() const { return diagnostics.empty(); }
};

[[nodiscard]] PackageManifestParseResult ParsePackageManifest(
    std::string_view json);
[[nodiscard]] std::string SerializePackageManifest(
    const PackageManifest& manifest);

}  // namespace px::sdk::detail
