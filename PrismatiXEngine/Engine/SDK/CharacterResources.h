#pragma once

#include "Engine/SDK/ContractVersions.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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
    // Message-window face for this expression.
    std::optional<std::string> faceAssetId;
    std::optional<std::string> faceAssetPath;
};

// The speaker name's look in the message window (`nameStyle`). Every field
// is optional and overrides only that part of the HUD's speaker label.
// Colours are "#RRGGBB" or "#RRGGBBAA"; empty strings are unset.
struct CharacterNameStyle {
    std::string color;
    std::string gradientFrom;
    std::string gradientTo;
    bool gradientHorizontal = false;
    std::string outlineColor;
    std::optional<int> outlineSize;
    std::string shadowColor;
    float shadowX = 2.0f;
    float shadowY = 2.0f;
    float shadowBlur = 0.0f;
    // A project font for the name only.
    std::string font;
    // Faux bold in pixels per side; negative is `true`: from the font size.
    std::optional<float> bold;
    // Faux italic in degrees.
    std::optional<float> italic;
    // The character's name plate picture (dialogue.speakerPlate).
    std::string plate;
};

struct CharacterResource {
    std::string id;
    std::string displayName;
    std::unordered_map<std::string, std::string> displayNames;
    // Speaker name colour in the message window; empty keeps the HUD's.
    std::string nameColor;
    std::optional<CharacterNameStyle> nameStyle;
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
