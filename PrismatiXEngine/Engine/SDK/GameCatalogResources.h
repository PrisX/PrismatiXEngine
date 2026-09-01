#pragma once

#include "Engine/SDK/ContractVersions.h"

#include <functional>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <variant>

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
    enum class Type : std::uint8_t { Boolean, Integer, Number, String };
    enum class Scope : std::uint8_t { Session, Profile };
    Type type = Type::Integer;
    Scope scope = Scope::Session;
    std::variant<bool, std::int64_t, double, std::string> typedDefault =
        std::int64_t{0};
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

struct GameCatalogUnlockable {
    std::string id;
    std::string kind;
    std::string condition;
    std::string payloadJson;
};

struct GameCatalogResourcesDocument {
    std::string documentId;
    std::vector<GameCatalogVariable> variables;
    std::vector<GameCatalogGalleryItem> gallery;
    std::vector<GameCatalogInputBinding> inputBindings;
    std::vector<std::string> progressionFlags;
    std::vector<GameCatalogUnlockable> unlockables;
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

// Loads the canonical PrismatiXGame JSON document used by 0.2 packages.
[[nodiscard]] GameCatalogResourcesLoadResult LoadCanonicalGameCatalogResources(
    std::string_view json,
    std::string path = "Content/game.pxgame");

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
