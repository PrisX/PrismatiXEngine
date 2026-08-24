#pragma once

#include "Engine/SDK/ContractVersions.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::sdk {

struct CharacterResourceDiagnostic {
    std::string code;
    std::string message;
    std::string path;
    std::string characterId;
    std::string expressionId;
};

struct CharacterResourceExpression {
    std::string id;
    std::string name;
    std::vector<std::string> aliases;
    std::string assetId;
    std::string assetPath;
    std::optional<std::string> thumbnailAssetId;
    std::optional<std::string> thumbnailAssetPath;
};

struct CharacterResource {
    std::string id;
    std::string displayName;
    // Compatibility lookup keys retained by the one-time Game.pxres
    // migration. The UUID remains authoritative.
    std::vector<std::string> aliases;
    std::string voiceDirectory;
    std::optional<std::string> defaultExpressionId;
    std::vector<CharacterResourceExpression> expressions;
};

struct CharacterResourcesDocument {
    std::vector<CharacterResource> characters;
};

struct CharacterResourcesLoadResult {
    // False means the project predates this contract and may use the one-time
    // legacy Game.pxres fallback. A present but invalid `characters` member is
    // always a hard failure and never falls back.
    bool declared = false;
    CharacterResourcesDocument document;
    std::vector<CharacterResourceDiagnostic> diagnostics;

    [[nodiscard]] bool Valid() const {
        return declared && diagnostics.empty();
    }
};

using CharacterResourceReadText =
    std::function<std::optional<std::string>(std::string_view uri)>;
using CharacterResourceExists =
    std::function<bool(std::string_view uri)>;

// Parses the project manifest's `characters` descriptors, loads every
// Characters/<uuid>.pxcharacter document through the caller's VFS, and
// validates all stable identities and asset references. PreviewHost, Player,
// and Packager use this single implementation.
[[nodiscard]] CharacterResourcesLoadResult LoadCharacterResources(
    std::string_view projectManifest,
    const CharacterResourceReadText& readText,
    const CharacterResourceExists& exists);

}  // namespace px::sdk
