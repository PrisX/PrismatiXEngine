#pragma once

#include "Engine/SDK/ContractVersions.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::sdk {

enum class LegacyGameCatalogPolicy {
    AllowCharacterNodes,
    RejectCharacterNodes,
};

enum class LegacyGalleryReferencePolicy {
    AllowPathStrings,
    RejectPathStrings,
};

struct GameCatalogResourceDiagnostic {
    std::string code;
    std::string message;
    std::string path;
    std::string nodeId;
    std::string property;
    int line = 0;
};

struct GameCatalogVariable {
    std::string nodeId;
    std::string name;
    int defaultValue = 0;
    bool persistent = false;
};

struct GameCatalogAssetReference {
    std::string assetId;
    std::string assetPath;
};

struct GameCatalogGalleryItem {
    std::string nodeId;
    std::string id;
    std::string title;
    GameCatalogAssetReference image;
    std::optional<GameCatalogAssetReference> thumbnail;
    int sourceLine = 0;
};

struct GameCatalogInputBinding {
    std::string nodeId;
    std::string key;
    std::string command;
    std::string argument;
};

struct GameCatalogResourcesDocument {
    std::string documentId;
    std::vector<GameCatalogVariable> variables;
    std::vector<GameCatalogGalleryItem> gallery;
    std::vector<GameCatalogInputBinding> inputBindings;
};

struct GameCatalogResourcesLoadResult {
    GameCatalogResourcesDocument document;
    bool legacyCharacterNodesPresent = false;
    std::vector<GameCatalogResourceDiagnostic> diagnostics;

    [[nodiscard]] bool Valid() const { return diagnostics.empty(); }
};

using GameCatalogResourceExists =
    std::function<bool(std::string_view uri)>;

// Loads the post-character-migration runtime semantics that remain in
// Content/Game.pxres. Unknown properties on supported entries are deliberately
// ignored so the one-time migration can preserve authoring metadata losslessly.
// PreviewHost, Player, and Packager use this single contract.
[[nodiscard]] GameCatalogResourcesLoadResult LoadGameCatalogResources(
    std::string_view typedResource,
    std::string path = "Content/Game.pxres",
    LegacyGameCatalogPolicy legacyPolicy =
        LegacyGameCatalogPolicy::RejectCharacterNodes,
    LegacyGalleryReferencePolicy galleryPolicy =
        LegacyGalleryReferencePolicy::RejectPathStrings);

// Resolves UUID-authoritative GalleryItem references through the project
// manifest and validates image kind and file availability. The manifest path
// replaces the serialized path hint after relocation; callers never trust the
// hint as identity. PreviewHost, Player, and Packager share this boundary.
[[nodiscard]] GameCatalogResourcesLoadResult ResolveGameCatalogGalleryResources(
    GameCatalogResourcesDocument document,
    std::string_view projectManifest,
    const GameCatalogResourceExists& exists,
    std::string path = "Content/Game.pxres");

}  // namespace px::sdk
