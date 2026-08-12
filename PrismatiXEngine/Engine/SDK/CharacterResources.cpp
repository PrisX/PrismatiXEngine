#include "Engine/SDK/CharacterResources.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>

namespace px::sdk {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxProjectManifestBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxCharacterBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxCharacters = 100'000;
constexpr std::size_t kMaxAssets = 1'000'000;
constexpr std::size_t kMaxExpressionsPerCharacter = 100'000;

struct AssetDescriptor {
    std::string path;
    std::string kind;
};

void Add(CharacterResourcesLoadResult& result, std::string code,
         std::string message, std::string path = {},
         std::string characterId = {}, std::string expressionId = {}) {
    result.diagnostics.push_back(
        {.code = std::move(code),
         .message = std::move(message),
         .path = std::move(path),
         .characterId = std::move(characterId),
         .expressionId = std::move(expressionId)});
}

bool IsUuid(const std::string_view value) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-') {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) continue;
        if (std::isxdigit(static_cast<unsigned char>(value[index])) == 0)
            return false;
    }
    return true;
}

bool IsSafeUri(const std::string_view uri) {
    if (uri.empty() || uri.size() > 4096 || uri.front() == '/' ||
        uri.find('\\') != std::string_view::npos ||
        uri.find(':') != std::string_view::npos) {
        return false;
    }
    std::size_t start = 0;
    while (start < uri.size()) {
        const auto end = uri.find('/', start);
        const auto component = uri.substr(
            start, end == std::string_view::npos ? uri.size() - start
                                                  : end - start);
        if (component.empty() || component == "." || component == "..")
            return false;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

bool NonEmptyText(const Json& object, const char* key, std::string& value,
                  const std::size_t maximum = 4096) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) return false;
    value = found->get<std::string>();
    return !value.empty() && value.size() <= maximum;
}

bool StringEquals(const Json& object, const char* key,
                  const std::string_view expected) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() &&
           found->get_ref<const std::string&>() == expected;
}

bool IntegerEquals(const Json& object, const char* key,
                   const std::uint64_t expected) {
    const auto found = object.find(key);
    if (found == object.end()) return false;
    if (found->is_number_unsigned())
        return found->get<std::uint64_t>() == expected;
    if (found->is_number_integer()) {
        const auto value = found->get<std::int64_t>();
        return value >= 0 && static_cast<std::uint64_t>(value) == expected;
    }
    return false;
}

std::optional<std::string> NullableUuid(const Json& object, const char* key,
                                        bool& valid) {
    const auto found = object.find(key);
    if (found == object.end() || found->is_null()) {
        valid = true;
        return std::nullopt;
    }
    if (!found->is_string()) {
        valid = false;
        return std::nullopt;
    }
    auto value = found->get<std::string>();
    valid = IsUuid(value);
    return valid ? std::optional<std::string>(std::move(value)) : std::nullopt;
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

}  // namespace

CharacterResourcesLoadResult LoadCharacterResources(
    const std::string_view projectManifest,
    const CharacterResourceReadText& readText,
    const CharacterResourceExists& exists) {
    CharacterResourcesLoadResult result;
    if (projectManifest.size() > kMaxProjectManifestBytes) {
        Add(result, "PXCHAR1000",
            "project.pxproject exceeds the 16 MiB characterResources contract limit",
            "project.pxproject");
        return result;
    }
    const Json manifest = Json::parse(projectManifest, nullptr, false);
    if (manifest.is_discarded() || !manifest.is_object()) {
        Add(result, "PXCHAR1001", "project.pxproject is not valid JSON",
            "project.pxproject");
        return result;
    }
    if (!StringEquals(manifest, "format", "PrismatiXProject") ||
        !IntegerEquals(manifest, "schemaRevision", 1)) {
        Add(result, "PXCHAR1002",
            "project.pxproject must be PrismatiXProject schema revision 1",
            "project.pxproject");
        return result;
    }
    const auto characters = manifest.find("characters");
    if (characters == manifest.end()) {
        return result;
    }
    result.declared = true;
    if (!characters->is_array() || characters->size() > kMaxCharacters) {
        Add(result, "PXCHAR1003",
            "characters must be an array within the contract limit",
            "project.pxproject");
        return result;
    }

    std::unordered_map<std::string, AssetDescriptor> assets;
    const auto assetList = manifest.find("assets");
    if (assetList == manifest.end() || !assetList->is_array() ||
        assetList->size() > kMaxAssets) {
        Add(result, "PXCHAR1004",
            "assets must be an array within the contract limit",
            "project.pxproject");
        return result;
    }
    for (const auto& asset : *assetList) {
        std::string id;
        std::string source;
        std::string kind;
        if (!asset.is_object() || !NonEmptyText(asset, "id", id, 36) ||
            !IsUuid(id) || !NonEmptyText(asset, "source", source) ||
            !IsSafeUri(source) || !NonEmptyText(asset, "kind", kind, 64)) {
            Add(result, "PXCHAR1005",
                "asset descriptors require a UUID id, safe source, and kind",
                "project.pxproject");
            continue;
        }
        if (!assets.emplace(id, AssetDescriptor{source, kind}).second) {
            Add(result, "PXCHAR1006", "duplicate asset UUID: " + id,
                "project.pxproject");
        }
    }

    std::unordered_set<std::string> characterIds;
    std::unordered_set<std::string> characterSources;
    std::unordered_set<std::string> expressionIds;
    for (const auto& descriptor : *characters) {
        std::string id;
        std::string displayName;
        std::string source;
        if (!descriptor.is_object() ||
            !NonEmptyText(descriptor, "id", id, 36) || !IsUuid(id) ||
            !NonEmptyText(descriptor, "displayName", displayName, 256) ||
            !NonEmptyText(descriptor, "source", source) ||
            !IsSafeUri(source) || !source.ends_with(".pxcharacter")) {
            Add(result, "PXCHAR1007",
                "character descriptors require UUID id, displayName, and a safe .pxcharacter source",
                "project.pxproject");
            continue;
        }
        if (!characterIds.insert(id).second) {
            Add(result, "PXCHAR1008", "duplicate character UUID: " + id,
                "project.pxproject", id);
            continue;
        }
        if (!characterSources.insert(Lower(source)).second) {
            Add(result, "PXCHAR1009",
                "duplicate character source: " + source,
                "project.pxproject", id);
            continue;
        }
        const std::string expected = "Characters/" + id + ".pxcharacter";
        if (Lower(source) != Lower(expected)) {
            Add(result, "PXCHAR1010",
                "character source must match its stable UUID: " + expected,
                source, id);
            continue;
        }
        const auto text = readText ? readText(source) : std::nullopt;
        if (!text) {
            Add(result, "PXCHAR1011",
                "character document is missing: " + source, source, id);
            continue;
        }
        if (text->size() > kMaxCharacterBytes) {
            Add(result, "PXCHAR1012",
                "character document exceeds the 4 MiB contract limit", source,
                id);
            continue;
        }
        const Json document = Json::parse(*text, nullptr, false);
        if (document.is_discarded() || !document.is_object()) {
            Add(result, "PXCHAR1013",
                "character document is not valid JSON", source, id);
            continue;
        }
        std::string documentId;
        std::string documentName;
        if (!StringEquals(document, "format", "PrismatiXCharacter") ||
            !IntegerEquals(document, "schemaRevision",
                           kCharacterResourcesContractRevision) ||
            !NonEmptyText(document, "id", documentId, 36) ||
            documentId != id ||
            !NonEmptyText(document, "displayName", documentName, 256) ||
            documentName != displayName) {
            Add(result, "PXCHAR1014",
                "character format, revision, identity, or displayName does not match its descriptor",
                source, id);
            continue;
        }

        CharacterResource character;
        character.id = id;
        character.displayName = displayName;
        const auto aliases = document.find("aliases");
        if (aliases != document.end()) {
            if (!aliases->is_array() || aliases->size() > 128) {
                Add(result, "PXCHAR1026",
                    "aliases must be an array with at most 128 entries",
                    source, id);
                continue;
            }
            std::unordered_set<std::string> localAliases;
            bool aliasesValid = true;
            for (const auto& alias : *aliases) {
                if (!alias.is_string()) {
                    aliasesValid = false;
                    break;
                }
                auto value = alias.get<std::string>();
                if (value.empty() || value.size() > 256 ||
                    std::any_of(value.begin(), value.end(),
                                [](const unsigned char character) {
                                    return character < 0x20;
                                }) ||
                    value == id || value == displayName ||
                    !localAliases.insert(value).second) {
                    aliasesValid = false;
                    break;
                }
                character.aliases.push_back(std::move(value));
            }
            if (!aliasesValid) {
                Add(result, "PXCHAR1027",
                    "aliases must be unique bounded text distinct from the UUID and displayName",
                    source, id);
                continue;
            }
        }
        const auto voice = document.find("voiceDirectory");
        if (voice != document.end()) {
            if (!voice->is_string() || voice->get<std::string>().size() > 4096 ||
                (!voice->get<std::string>().empty() &&
                 !IsSafeUri(voice->get<std::string>()))) {
                Add(result, "PXCHAR1015",
                    "voiceDirectory must be empty or a safe project-relative path",
                    source, id);
                continue;
            }
            character.voiceDirectory = voice->get<std::string>();
        }
        bool defaultValid = false;
        character.defaultExpressionId =
            NullableUuid(document, "defaultExpressionId", defaultValid);
        if (!defaultValid) {
            Add(result, "PXCHAR1016",
                "defaultExpressionId must be null or a UUID", source, id);
            continue;
        }
        const auto expressions = document.find("expressions");
        if (expressions == document.end() || !expressions->is_array() ||
            expressions->size() > kMaxExpressionsPerCharacter) {
            Add(result, "PXCHAR1017",
                "expressions must be an array within the contract limit", source,
                id);
            continue;
        }
        std::unordered_set<std::string> names;
        bool characterValid = true;
        for (const auto& expression : *expressions) {
            CharacterResourceExpression value;
            if (!expression.is_object() ||
                !NonEmptyText(expression, "id", value.id, 36) ||
                !IsUuid(value.id) ||
                !NonEmptyText(expression, "name", value.name, 256) ||
                !NonEmptyText(expression, "assetId", value.assetId, 36) ||
                !IsUuid(value.assetId)) {
                Add(result, "PXCHAR1018",
                    "expressions require UUID id, name, and assetId", source,
                    id);
                characterValid = false;
                continue;
            }
            if (!expressionIds.insert(value.id).second ||
                !names.insert(Lower(value.name)).second) {
                Add(result, "PXCHAR1019",
                    "expression UUIDs and case-insensitive names must be unique",
                    source, id, value.id);
                characterValid = false;
                continue;
            }
            const auto expressionAliases = expression.find("aliases");
            if (expressionAliases != expression.end()) {
                if (!expressionAliases->is_array() ||
                    expressionAliases->size() > 128) {
                    Add(result, "PXCHAR1029",
                        "expression aliases must be an array with at most 128 entries",
                        source, id, value.id);
                    characterValid = false;
                    continue;
                }
                std::unordered_set<std::string> localAliases;
                bool aliasesValid = true;
                for (const auto& alias : *expressionAliases) {
                    if (!alias.is_string()) {
                        aliasesValid = false;
                        break;
                    }
                    auto aliasValue = alias.get<std::string>();
                    if (aliasValue.empty() || aliasValue.size() > 256 ||
                        std::any_of(
                            aliasValue.begin(), aliasValue.end(),
                            [](const unsigned char character) {
                                return character < 0x20;
                            }) ||
                        aliasValue == value.id || aliasValue == value.name ||
                        !localAliases.insert(aliasValue).second) {
                        aliasesValid = false;
                        break;
                    }
                    value.aliases.push_back(std::move(aliasValue));
                }
                if (!aliasesValid) {
                    Add(result, "PXCHAR1030",
                        "expression aliases must be unique bounded text distinct from UUID and name",
                        source, id, value.id);
                    characterValid = false;
                    continue;
                }
            }
            const auto asset = assets.find(value.assetId);
            if (asset == assets.end() ||
                (asset->second.kind != "character" &&
                 asset->second.kind != "cg" &&
                 asset->second.kind != "uiImage")) {
                Add(result, "PXCHAR1020",
                    "expression references a missing or incompatible asset: " +
                        value.assetId,
                    source, id, value.id);
                characterValid = false;
                continue;
            }
            if (!exists || !exists(asset->second.path)) {
                Add(result, "PXCHAR1021",
                    "expression asset file is missing: " + asset->second.path,
                    asset->second.path, id, value.id);
                characterValid = false;
                continue;
            }
            value.assetPath = asset->second.path;
            bool thumbnailValid = false;
            value.thumbnailAssetId =
                NullableUuid(expression, "thumbnailAssetId", thumbnailValid);
            if (!thumbnailValid) {
                Add(result, "PXCHAR1022",
                    "thumbnailAssetId must be null or a UUID", source, id,
                    value.id);
                characterValid = false;
                continue;
            }
            if (value.thumbnailAssetId) {
                const auto thumbnail = assets.find(*value.thumbnailAssetId);
                if (thumbnail == assets.end() ||
                    (thumbnail->second.kind != "character" &&
                     thumbnail->second.kind != "cg" &&
                     thumbnail->second.kind != "uiImage") ||
                    !exists ||
                    !exists(thumbnail->second.path)) {
                    Add(result, "PXCHAR1023",
                        "expression thumbnail asset is missing: " +
                            *value.thumbnailAssetId,
                        source, id, value.id);
                    characterValid = false;
                    continue;
                }
                value.thumbnailAssetPath = thumbnail->second.path;
            }
            character.expressions.push_back(std::move(value));
        }
        if (!characterValid) continue;
        if (!character.expressions.empty() &&
            !character.defaultExpressionId) {
            Add(result, "PXCHAR1024",
                "a character with expressions requires defaultExpressionId",
                source, id);
            continue;
        }
        if (character.defaultExpressionId &&
            std::none_of(character.expressions.begin(),
                         character.expressions.end(), [&](const auto& item) {
                             return item.id == *character.defaultExpressionId;
                         })) {
            Add(result, "PXCHAR1025",
                "defaultExpressionId does not reference an expression", source,
                id, *character.defaultExpressionId);
            continue;
        }
        result.document.characters.push_back(std::move(character));
    }
    std::unordered_map<std::string, std::string> lookupOwners;
    for (const auto& character : result.document.characters) {
        for (const auto& key :
             std::vector<std::string>{character.id, character.displayName}) {
            const auto [found, inserted] =
                lookupOwners.emplace(key, character.id);
            if (!inserted && found->second != character.id) {
                Add(result, "PXCHAR1028",
                    "runtime character lookup key is ambiguous: " + key,
                    "project.pxproject", character.id);
            }
        }
        for (const auto& alias : character.aliases) {
            const auto [found, inserted] =
                lookupOwners.emplace(alias, character.id);
            if (!inserted && found->second != character.id) {
                Add(result, "PXCHAR1028",
                    "runtime character alias is ambiguous: " + alias,
                    "project.pxproject", character.id);
            }
        }
        std::unordered_map<std::string, std::string> expressionLookupOwners;
        for (const auto& expression : character.expressions) {
            for (const auto& key :
                 std::vector<std::string>{expression.id, expression.name}) {
                const auto [found, inserted] =
                    expressionLookupOwners.emplace(key, expression.id);
                if (!inserted && found->second != expression.id) {
                    Add(result, "PXCHAR1031",
                        "runtime expression lookup key is ambiguous: " + key,
                        "project.pxproject", character.id, expression.id);
                }
            }
            for (const auto& alias : expression.aliases) {
                const auto [found, inserted] =
                    expressionLookupOwners.emplace(alias, expression.id);
                if (!inserted && found->second != expression.id) {
                    Add(result, "PXCHAR1031",
                        "runtime expression alias is ambiguous: " + alias,
                        "project.pxproject", character.id, expression.id);
                }
            }
        }
    }
    return result;
}

}  // namespace px::sdk
