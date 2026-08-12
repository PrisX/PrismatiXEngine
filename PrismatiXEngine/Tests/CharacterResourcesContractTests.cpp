#include "Engine/SDK/CharacterResources.h"
#include "Engine/VN/GameCatalog.h"
#include "Tests/TestSupport/TestHarness.h"

#include <string>
#include <unordered_map>

namespace {

constexpr std::string_view kCharacterId =
    "11111111-1111-4111-8111-111111111111";
constexpr std::string_view kExpressionId =
    "22222222-2222-4222-8222-222222222222";
constexpr std::string_view kAssetId =
    "33333333-3333-4333-8333-333333333333";
constexpr std::string_view kCharacterPath =
    "Characters/11111111-1111-4111-8111-111111111111.pxcharacter";
constexpr std::string_view kAssetPath = "Assets/rin.png";

std::string Manifest(const std::string_view characters = R"([
    {"id":"11111111-1111-4111-8111-111111111111",
     "displayName":"凛",
     "source":"Characters/11111111-1111-4111-8111-111111111111.pxcharacter"}
  ])") {
    return std::string(R"({
  "format":"PrismatiXProject",
  "schemaRevision":1,
  "assets":[
    {"id":"33333333-3333-4333-8333-333333333333",
     "kind":"character","source":"Assets/rin.png"}
  ],
  "characters":)") +
           std::string(characters) + "}";
}

std::string Character(
    const std::string_view defaultExpression =
        R"("22222222-2222-4222-8222-222222222222")",
    const std::string_view expressions = R"([
    {"id":"22222222-2222-4222-8222-222222222222",
     "name":"Neutral",
     "aliases":["neutral"],
     "assetId":"33333333-3333-4333-8333-333333333333",
     "thumbnailAssetId":null}
  ])") {
    return std::string(R"({
  "format":"PrismatiXCharacter",
  "schemaRevision":1,
  "id":"11111111-1111-4111-8111-111111111111",
  "revision":0,
  "displayName":"凛",
  "aliases":["rin"],
  "voiceDirectory":"Content/Audio/Voice/Rin",
  "defaultExpressionId":)") +
           std::string(defaultExpression) + R"(,
  "expressions":)" + std::string(expressions) + "}";
}

px::sdk::CharacterResourcesLoadResult Load(
    const std::string& manifest,
    std::unordered_map<std::string, std::string> files) {
    return px::sdk::LoadCharacterResources(
        manifest,
        [files](const std::string_view uri) -> std::optional<std::string> {
            const auto found = files.find(std::string(uri));
            return found == files.end()
                       ? std::nullopt
                       : std::optional<std::string>{found->second};
        },
        [files](const std::string_view uri) {
            return files.contains(std::string(uri));
        });
}

bool HasCode(const px::sdk::CharacterResourcesLoadResult& result,
             const std::string_view code) {
    for (const auto& diagnostic : result.diagnostics)
        if (diagnostic.code == code) return true;
    return false;
}

}  // namespace

int main() {
    px::test::Suite suite("CharacterResourcesContract");

    suite.Run("LoadsStableRuntimeSemantics", [&] {
        const auto loaded =
            Load(Manifest(), {{std::string(kCharacterPath), Character()},
                              {std::string(kAssetPath), "png"}});
        suite.Require(loaded.Valid(),
                      "characterResources revision 1 loads");
        suite.Require(loaded.document.characters.size() == 1,
                      "one character is retained");
        const auto& character = loaded.document.characters.front();
        suite.Expect(
            character.id == kCharacterId && character.displayName == "凛" &&
                character.aliases.size() == 1 &&
                character.aliases.front() == "rin" &&
                character.voiceDirectory == "Content/Audio/Voice/Rin" &&
                character.defaultExpressionId == kExpressionId,
            "stable character identity and runtime presentation survive");
        suite.Require(character.expressions.size() == 1,
                      "one expression is retained");
        suite.Expect(
            character.expressions.front().id == kExpressionId &&
                character.expressions.front().aliases ==
                    std::vector<std::string>{"neutral"} &&
                character.expressions.front().assetId == kAssetId &&
                character.expressions.front().assetPath == kAssetPath,
            "stable expression and resource UUIDs resolve to runtime paths");
    });

    suite.Run("LegacyManifestRemainsAnExplicitFallback", [&] {
        const auto loaded = Load(
            R"({"format":"PrismatiXProject","schemaRevision":1,"assets":[]})",
            {});
        suite.Expect(!loaded.declared && loaded.diagnostics.empty(),
                     "missing characters member is not silently treated as the new contract");
    });

    suite.Run("RuntimeCatalogResolvesMigratedAliases", [&] {
        const std::unordered_map<std::string, std::string> files{
            {std::string(kCharacterPath), Character()},
            {std::string(kAssetPath), "png"}};
        px::vn::GameCatalog catalog;
        bool declared = false;
        const auto status = catalog.LoadCharacterResources(
            Manifest(),
            [&files](const std::string_view uri)
                -> std::optional<std::string> {
                const auto found = files.find(std::string(uri));
                return found == files.end()
                           ? std::nullopt
                           : std::optional<std::string>{found->second};
            },
            [&files](const std::string_view uri) {
                return files.contains(std::string(uri));
            },
            declared);
        suite.Require(status.IsOk() && declared,
                      "runtime adapter accepts characterResources revision 1");
        const auto* character = catalog.FindCharacter("rin");
        suite.Require(character != nullptr && character->id == kCharacterId,
                      "legacy character alias resolves to stable UUID");
        suite.Expect(catalog.FindCharacter("RIN") == nullptr,
                     "legacy lookup remains exact-case like GameCatalog");
        const auto* expression =
            catalog.FindExpression(*character, "neutral");
        suite.Expect(expression != nullptr &&
                         expression->id == kExpressionId &&
                         expression->image.id.ToString() == kAssetId,
                     "legacy expression alias resolves to stable expression and resource UUID");
    });

    suite.Run("MissingAndMalformedCharacterAreDiagnosed", [&] {
        const auto missing =
            Load(Manifest(), {{std::string(kAssetPath), "png"}});
        suite.Expect(HasCode(missing, "PXCHAR1011"),
                     "missing character document is explicit");
        const auto malformed =
            Load(Manifest(),
                 {{std::string(kCharacterPath), "{"},
                  {std::string(kAssetPath), "png"}});
        suite.Expect(HasCode(malformed, "PXCHAR1013"),
                     "malformed character document is explicit");
    });

    suite.Run("WrongJsonTypesNeverEscapeAsExceptions", [&] {
        const auto manifestTypes = Load(
            R"({"format":42,"schemaRevision":"1","assets":[],"characters":[]})",
            {});
        suite.Expect(HasCode(manifestTypes, "PXCHAR1002"),
                     "wrong project format/revision types are diagnosed");
        const auto characterTypes = Load(
            Manifest(),
            {{std::string(kCharacterPath),
              R"({"format":{},"schemaRevision":"1",
                   "id":"11111111-1111-4111-8111-111111111111",
                   "displayName":"凛","expressions":[]})"},
             {std::string(kAssetPath), "png"}});
        suite.Expect(HasCode(characterTypes, "PXCHAR1014"),
                     "wrong character format/revision types are diagnosed");
    });

    suite.Run("DuplicateStableIdentitiesAreDiagnosed", [&] {
        const auto duplicateCharacters = std::string(R"([
          {"id":"11111111-1111-4111-8111-111111111111","displayName":"凛",
           "source":"Characters/11111111-1111-4111-8111-111111111111.pxcharacter"},
          {"id":"11111111-1111-4111-8111-111111111111","displayName":"凛",
           "source":"Characters/11111111-1111-4111-8111-111111111111.pxcharacter"}
        ])");
        const auto loaded =
            Load(Manifest(duplicateCharacters),
                 {{std::string(kCharacterPath), Character()},
                  {std::string(kAssetPath), "png"}});
        suite.Expect(HasCode(loaded, "PXCHAR1008"),
                     "duplicate character UUID is explicit");
    });

    suite.Run("DefaultAndAssetFailuresAreDiagnosed", [&] {
        const auto invalidDefault =
            Load(Manifest(),
                 {{std::string(kCharacterPath),
                   Character(
                       R"("44444444-4444-4444-8444-444444444444")")},
                  {std::string(kAssetPath), "png"}});
        suite.Expect(HasCode(invalidDefault, "PXCHAR1025"),
                     "missing default expression is explicit");
        const auto missingAsset =
            Load(Manifest(),
                 {{std::string(kCharacterPath), Character()}});
        suite.Expect(HasCode(missingAsset, "PXCHAR1021"),
                     "missing expression asset file is explicit");
        const auto thumbnailKind = Load(
            R"({
              "format":"PrismatiXProject","schemaRevision":1,
              "assets":[
                {"id":"33333333-3333-4333-8333-333333333333",
                 "kind":"character","source":"Assets/rin.png"},
                {"id":"55555555-5555-4555-8555-555555555555",
                 "kind":"audio","source":"Assets/rin.ogg"}
              ],
              "characters":[{
                "id":"11111111-1111-4111-8111-111111111111",
                "displayName":"凛",
                "source":"Characters/11111111-1111-4111-8111-111111111111.pxcharacter"
              }]
            })",
            {{std::string(kCharacterPath),
              R"({
                "format":"PrismatiXCharacter","schemaRevision":1,
                "id":"11111111-1111-4111-8111-111111111111",
                "displayName":"凛",
                "defaultExpressionId":"22222222-2222-4222-8222-222222222222",
                "expressions":[{
                  "id":"22222222-2222-4222-8222-222222222222",
                  "name":"Neutral",
                  "assetId":"33333333-3333-4333-8333-333333333333",
                  "thumbnailAssetId":"55555555-5555-4555-8555-555555555555"
                }]
              })"},
             {std::string(kAssetPath), "png"},
             {"Assets/rin.ogg", "audio"}});
        suite.Expect(HasCode(thumbnailKind, "PXCHAR1023"),
                     "incompatible thumbnail asset kind is explicit");
    });

    return suite.Finish();
}
