#include <string>

#include <nlohmann/json.hpp>

#include "Engine/SDK/GameCatalogResources.h"
#include "Engine/VN/GameCatalog.h"
#include "Tests/TestSupport/TestHarness.h"

namespace {

constexpr std::string_view kResidualCatalog = R"(
@pxresource 4 00000000-0000-4000-8000-000000000001 GameCatalog

[node "00000000-0000-4000-8000-000000000010", "", "OpenSettings", "InputBinding"]
key = "Escape"
command = "screen.open"
argument = "settings"
platform = "desktop"

[node "00000000-0000-4000-8000-000000000011", "", "Affinity", "Variable"]
name = "affinity"
default = 7
persistent = true
editorMetadata = object("group", "routes")

[node "00000000-0000-4000-8000-000000000012", "", "EndingCG", "GalleryItem"]
id = "ending-rin"
title = "Rin Ending"
image = "Content/CG/rin-ending.png"
thumbnail = "Content/CG/rin-ending-thumb.png"
sourceAsset = res("00000000-0000-4000-8000-000000000013", "Content/CG/rin-ending.png")
)";

constexpr std::string_view kCanonicalResidualCatalog = R"(
@pxresource 4 00000000-0000-4000-8000-000000000001 GameCatalog

[node "00000000-0000-4000-8000-000000000010", "", "OpenSettings", "InputBinding"]
key = "Escape"
command = "screen.open"
argument = "settings"

[node "00000000-0000-4000-8000-000000000011", "", "Affinity", "Variable"]
name = "affinity"
default = 7
persistent = true

[node "00000000-0000-4000-8000-000000000012", "", "EndingCG", "GalleryItem"]
id = "ending-rin"
title = "Rin Ending"
image = res("00000000-0000-4000-8000-000000000013", "Content/Old/rin-ending.png")
thumbnail = res("00000000-0000-4000-8000-000000000014", "Content/Old/rin-ending-thumb.png")
)";

constexpr std::string_view kProjectManifest = R"({
  "format":"PrismatiXProject",
  "schemaRevision":1,
  "assets":[
    {"id":"00000000-0000-4000-8000-000000000013","kind":"cg","source":"Content/CG/rin-ending.png"},
    {"id":"00000000-0000-4000-8000-000000000014","kind":"uiImage","source":"Content/CG/rin-ending-thumb.png"}
  ]
})";

constexpr std::string_view kCanonicalCatalog = R"({
  "format":"PrismatiXGame",
  "schemaRevision":1,
  "variables":[
    {"name":"skipRead","type":"boolean","default":true,"scope":"profile"},
    {"name":"affinity","type":"integer","default":7,"scope":"session"},
    {"name":"textSpeed","type":"number","default":1.25,"scope":"profile"},
    {"name":"routeName","type":"string","default":"rin","scope":"session"}
  ],
  "progressionFlags":["ending.rin"],
  "gallery":[],
  "unlockables":[
    {"id":"extra.rin","kind":"route","condition":"ending.rin","payload":{"route":"rin","chapter":2}}
  ],
  "inputBindings":[
    {"key":"Escape","command":"screen.open","argument":"settings"}
  ],
  "inputActions":[
    {"id":"quick-save","context":"game","keys":["F8"],"wheel":{"direction":"none"}},
    {"id":"screen.open.gallery","context":"game","keys":["G"],"wheel":{"direction":"up","threshold":1},"route":"gallery"}
  ]
})";

bool HasCode(const px::sdk::GameCatalogResourcesLoadResult& result, const std::string_view code) {
    for (const auto& diagnostic : result.diagnostics)
        if (diagnostic.code == code) return true;
    return false;
}

}  // namespace

int main() {
    px::test::Suite suite("GameCatalogResourcesContract");

    suite.Run("CanonicalCatalogPreservesTypedValuesAndScopes", [&] {
        const auto loaded =
            px::sdk::LoadCanonicalGameCatalogResources(kCanonicalCatalog);
        suite.Require(loaded.Valid(), "canonical 0.2 GameCatalog loads");
        suite.Require(loaded.document.variables.size() == 4,
                      "all four public scalar types are retained");
        const auto& boolean = loaded.document.variables[0];
        const auto& integer = loaded.document.variables[1];
        const auto& number = loaded.document.variables[2];
        const auto& string = loaded.document.variables[3];
        suite.Expect(
            boolean.type == px::sdk::GameCatalogVariable::Type::Boolean &&
                boolean.scope == px::sdk::GameCatalogVariable::Scope::Profile &&
                std::get<bool>(boolean.typedDefault),
            "boolean profile defaults remain typed");
        suite.Expect(
            integer.type == px::sdk::GameCatalogVariable::Type::Integer &&
                integer.scope == px::sdk::GameCatalogVariable::Scope::Session &&
                std::get<std::int64_t>(integer.typedDefault) == 7,
            "integer session defaults remain typed");
        suite.Expect(
            number.type == px::sdk::GameCatalogVariable::Type::Number &&
                number.scope == px::sdk::GameCatalogVariable::Scope::Profile &&
                std::get<double>(number.typedDefault) == 1.25,
            "number profile defaults remain typed");
        suite.Expect(
            string.type == px::sdk::GameCatalogVariable::Type::String &&
                string.scope == px::sdk::GameCatalogVariable::Scope::Session &&
                std::get<std::string>(string.typedDefault) == "rin",
            "string session defaults remain typed");
        suite.Expect(
            loaded.document.progressionFlags.size() == 1 &&
                loaded.document.unlockables.size() == 1 &&
                loaded.document.unlockables.front().payloadJson ==
                    R"({"chapter":2,"route":"rin"})" &&
                loaded.document.inputBindings.size() == 1,
            "progression, unlockable payloads, and input bindings survive canonical lowering");
        suite.Expect(loaded.document.inputActions.size() == 2 &&
                         loaded.document.inputActions[0].id == "quick-save" &&
                         loaded.document.inputActions[0].keys == std::vector<std::string>{"F8"} &&
                         loaded.document.inputActions[0].wheelDirection == "none" &&
                         loaded.document.inputActions[1].wheelDirection == "up" &&
                         loaded.document.inputActions[1].wheelThreshold == 1.0f &&
                         loaded.document.inputActions[1].route == "gallery",
                     "named actions, contexts, default keys, and routes survive canonical loading");

        px::vn::GameCatalog runtime;
        const auto status = runtime.LoadCanonical(
            kCanonicalCatalog,
            R"({"format":"PrismatiXProject","schemaRevision":1,"assets":[]})",
            [](std::string_view) { return false; });
        suite.Require(status.IsOk(), "runtime consumes the canonical catalog directly");
        suite.Expect(
            runtime.Variables().size() == 4 &&
                runtime.Variables()[0].scope ==
                    px::vn::CatalogVariable::Scope::Profile &&
                runtime.Variables()[0].typedDefault.TryGet<bool>() &&
                *runtime.Variables()[0].typedDefault.TryGet<bool>() &&
                runtime.Variables()[1].scope ==
                    px::vn::CatalogVariable::Scope::Session &&
                runtime.Variables()[2].typedDefault.TryGet<double>() &&
                *runtime.Variables()[2].typedDefault.TryGet<double>() == 1.25 &&
                runtime.Variables()[3].typedDefault.TryGet<std::string>() &&
                *runtime.Variables()[3].typedDefault.TryGet<std::string>() == "rin",
            "runtime lowering exposes the canonical executable variable semantics");
        suite.Expect(runtime.InputActions().size() == 2 &&
                         runtime.InputActions()[1].context == "game" &&
                         runtime.InputActions()[1].route == "gallery",
                     "runtime adapter exposes declarative input actions to hosts");
        suite.Expect(loaded.document.unlockables.size() == 1,
                     "declarative unlock metadata remains available to SDK tooling");
    });

    suite.Run("LoadsResidualRuntimeSemanticsAndStableIdentities", [&] {
        const auto loaded = px::sdk::LoadGameCatalogResources(
            kCanonicalResidualCatalog);
        suite.Require(loaded.Valid(), "post-migration Content/Game.pxres loads");
        suite.Expect(
            loaded.document.documentId == "00000000-0000-4000-8000-000000000001" && loaded.document.inputBindings.size() == 1 && loaded.document.inputBindings.front().nodeId == "00000000-0000-4000-8000-000000000010" &&
                loaded.document.inputBindings.front().key == "Escape" && loaded.document.inputBindings.front().command == "screen.open" && loaded.document.inputBindings.front().argument == "settings",
            "input binding runtime semantics and stable node identity survive"
        );
        suite.Expect(
            loaded.document.variables.size() == 1 && loaded.document.variables.front().name == "affinity" && loaded.document.variables.front().defaultValue == 7 && loaded.document.variables.front().persistent,
            "variable runtime defaults and persistence survive"
        );
        suite.Expect(
            loaded.document.gallery.size() == 1 && loaded.document.gallery.front().id == "ending-rin" && loaded.document.gallery.front().title == "Rin Ending" && loaded.document.gallery.front().image.assetId == "00000000-0000-4000-8000-000000000013" &&
                loaded.document.gallery.front().thumbnail && loaded.document.gallery.front().thumbnail->assetId == "00000000-0000-4000-8000-000000000014",
            "gallery UUID authority survives parsing"
        );
    });

    suite.Run("RuntimeAdapterUsesTheSameContract", [&] {
        px::vn::GameCatalog catalog;
        const auto status = catalog.LoadLegacyRuntimeResourcesForMigration(
            kCanonicalResidualCatalog, "Content/Game.pxres",
            px::sdk::LegacyGameCatalogPolicy::RejectCharacterNodes,
            px::sdk::LegacyGalleryReferencePolicy::RejectPathStrings,
            kProjectManifest,
            [](const std::string_view uri) {
                return uri == "Content/CG/rin-ending.png" ||
                       uri == "Content/CG/rin-ending-thumb.png";
            });
        suite.Require(status.IsOk(), "VN GameCatalog accepts the SDK contract");
        suite.Expect(catalog.InputBindings().size() == 1 && catalog.Variables().size() == 1 && catalog.Gallery().size() == 1 && catalog.Variables().front().defaultValue == 7, "Player and Preview runtime adapter retain all residual semantics");
        suite.Expect(
            catalog.Gallery().front().image ==
                    "Content/CG/rin-ending.png" &&
                catalog.Gallery().front().thumbnail ==
                    "Content/CG/rin-ending-thumb.png" &&
                catalog.Gallery().front().imageReference.id.ToString() ==
                    "00000000-0000-4000-8000-000000000013" &&
                catalog.Gallery().front().thumbnailReference &&
                catalog.Gallery().front().thumbnailReference->id.ToString() ==
                    "00000000-0000-4000-8000-000000000014",
            "runtime resolves relocated path hints from the UUID manifest authority");
    });

    suite.Run("UnknownAuthoringPropertiesRemainForwardCompatible", [&] {
        const auto loaded = px::sdk::LoadGameCatalogResources(
            kCanonicalResidualCatalog);
        suite.Expect(loaded.Valid(), "opaque authoring metadata does not become runtime-owned");
    });

    suite.Run("LegacyGalleryPathsRequireExplicitMigrationPolicy", [&] {
        const auto strict = px::sdk::LoadGameCatalogResources(kResidualCatalog);
        suite.Expect(HasCode(strict, "PXCAT1019"),
                     "new runtime contracts reject path-authoritative Gallery items");
        const auto legacy = px::sdk::LoadGameCatalogResources(
            kResidualCatalog, "Content/Game.pxres",
            px::sdk::LegacyGameCatalogPolicy::AllowCharacterNodes,
            px::sdk::LegacyGalleryReferencePolicy::AllowPathStrings);
        suite.Expect(
            legacy.Valid() && legacy.document.gallery.size() == 1 &&
                legacy.document.gallery.front().image.assetId.empty() &&
                legacy.document.gallery.front().image.assetPath ==
                    "Content/CG/rin-ending.png",
            "one-time legacy fallback is explicit and remains path-readable");
    });

    suite.Run("GalleryRuntimeRejectsUnknownKindAndMissingAssets", [&] {
        const auto parsed =
            px::sdk::LoadGameCatalogResources(kCanonicalResidualCatalog);
        suite.Require(parsed.Valid(), "canonical catalog parses before resolution");
        const auto unknown = px::sdk::ResolveGameCatalogGalleryResources(
            parsed.document,
            R"({"format":"PrismatiXProject","schemaRevision":1,"assets":[]})",
            [](std::string_view) { return true; });
        suite.Expect(HasCode(unknown, "PXCAT1024"),
                     "unknown Gallery UUIDs are rejected");

        const auto wrongKind = px::sdk::ResolveGameCatalogGalleryResources(
            parsed.document,
            R"({"format":"PrismatiXProject","schemaRevision":1,"assets":[{"id":"00000000-0000-4000-8000-000000000013","kind":"voice","source":"Content/CG/rin-ending.png"},{"id":"00000000-0000-4000-8000-000000000014","kind":"uiImage","source":"Content/CG/rin-ending-thumb.png"}]})",
            [](std::string_view) { return true; });
        suite.Expect(HasCode(wrongKind, "PXCAT1025"),
                     "non-image Gallery assets are rejected");

        const auto missing = px::sdk::ResolveGameCatalogGalleryResources(
            parsed.document, kProjectManifest,
            [](std::string_view) { return false; });
        suite.Expect(HasCode(missing, "PXCAT1026"),
                     "missing Gallery files are rejected");
    });

    suite.Run("CharacterResourcesProjectsRejectLegacyCharacterNodes", [&] {
        const auto loaded = px::sdk::LoadGameCatalogResources(R"(
@pxresource 4 00000000-0000-4000-8000-000000000001 GameCatalog
[node "00000000-0000-4000-8000-000000000020", "", "Rin", "Character"]
id = "rin"
name = "Rin"
)");
        suite.Expect(HasCode(loaded, "PXCAT1009"), "new characterResources ownership forbids a hidden dual source");

        const auto legacy = px::sdk::LoadGameCatalogResources(
            R"(
@pxresource 4 00000000-0000-4000-8000-000000000001 GameCatalog
[node "00000000-0000-4000-8000-000000000020", "", "Rin", "Character"]
id = "rin"
name = "Rin"
)",
            "Content/Game.pxres",
            px::sdk::LegacyGameCatalogPolicy::AllowCharacterNodes
        );
        suite.Expect(legacy.Valid() && legacy.legacyCharacterNodesPresent, "explicit legacy fallback remains available");
    });

    suite.Run("InvalidRuntimeFieldsAreDiagnosed", [&] {
        const auto loaded = px::sdk::LoadGameCatalogResources(R"(
@pxresource 4 00000000-0000-4000-8000-000000000001 GameCatalog
[node "00000000-0000-4000-8000-000000000010", "", "Bad", "Variable"]
name = "affinity"
default = "seven"
persistent = 1
[node "00000000-0000-4000-8000-000000000011", "", "BadGallery", "GalleryItem"]
id = "ending"
image = 42
)");
        suite.Expect(HasCode(loaded, "PXCAT1012") && HasCode(loaded, "PXCAT1013") && HasCode(loaded, "PXCAT1018"), "wrong integer, boolean, and ResourceRef types are explicit");
    });

    suite.Run("DuplicateRuntimeKeysAreRejected", [&] {
        const auto loaded = px::sdk::LoadGameCatalogResources(R"(
@pxresource 4 00000000-0000-4000-8000-000000000001 GameCatalog
[node "00000000-0000-4000-8000-000000000010", "", "One", "Variable"]
name = "affinity"
[node "00000000-0000-4000-8000-000000000011", "", "Two", "Variable"]
name = "affinity"
)");
        suite.Expect(HasCode(loaded, "PXCAT1016"), "ambiguous variable runtime identity is rejected");
    });

    suite.Run("InputActionsRejectDuplicatesAndMalformedKeys", [&] {
        constexpr std::string_view prefix = R"({"format":"PrismatiXGame","schemaRevision":1,"variables":[],"gallery":[],"unlockables":[],)";
        const auto duplicates = px::sdk::LoadCanonicalGameCatalogResources(
            std::string(prefix) + R"("inputBindings":[{"key":"G","command":"screen.open","argument":"gallery"}],"inputActions":[{"id":"screen.open.gallery","context":"game","keys":["F8"]}]})");
        suite.Expect(HasCode(duplicates, "PXCAT1119"),
                     "new and legacy action spellings cannot silently override each other");
        const auto malformed = px::sdk::LoadCanonicalGameCatalogResources(
            std::string(prefix) + R"("inputActions":[{"id":"quick-save","context":"game","keys":["F8","F8"]}]})");
        suite.Expect(HasCode(malformed, "PXCAT1120"),
                     "duplicate default key names fail before Player startup");
        const auto invalidWheel = px::sdk::LoadCanonicalGameCatalogResources(
            std::string(prefix) + R"("inputActions":[{"id":"advance","context":"game","wheel":{"direction":"none","threshold":1}}]})");
        suite.Expect(HasCode(invalidWheel, "PXCAT1121"),
                     "invalid wheel descriptors fail at the native boundary");
        const auto wrongContext = px::sdk::LoadCanonicalGameCatalogResources(
            std::string(prefix) + R"("inputActions":[{"id":"screen.open.gallery","context":"ui","route":"gallery"}]})");
        suite.Expect(HasCode(wrongContext, "PXCAT1119"),
                     "route shortcuts cannot be declared in a context the Player never checks");
        const auto clearBuiltin = px::sdk::LoadCanonicalGameCatalogResources(
            std::string(prefix) + R"("inputActions":[{"id":"toggle-skip","context":"game","keys":[]}]})");
        const auto retainBuiltin = px::sdk::LoadCanonicalGameCatalogResources(
            std::string(prefix) + R"("inputActions":[{"id":"toggle-skip","context":"game"}]})");
        suite.Expect(clearBuiltin.Valid() && retainBuiltin.Valid() &&
                         clearBuiltin.document.inputActions[0].keysSpecified &&
                         clearBuiltin.document.inputActions[0].keys.empty() &&
                         !retainBuiltin.document.inputActions[0].keysSpecified,
                     "empty authored keys clear defaults while omission retains them");
        px::vn::GameCatalog runtime;
        suite.Require(runtime.LoadCanonical(
            std::string(prefix) + R"("inputActions":[{"id":"toggle-skip","context":"game","keys":[]}]})",
            R"({"format":"PrismatiXProject","schemaRevision":1,"assets":[]})",
            [](std::string_view) { return false; }).IsOk(),
            "runtime accepts an explicitly empty keyboard default");
        suite.Expect(runtime.InputActions().size() == 1 &&
                         runtime.InputActions()[0].keysSpecified,
                     "empty-key intent survives catalog-to-Player transfer");
    });

    suite.Run("InputActionsPreserveUiCommands", [&] {
        constexpr std::string_view source = R"({
          "format":"PrismatiXGame","schemaRevision":1,
          "variables":[],"gallery":[],"unlockables":[],
          "inputActions":[{"id":"settings.keys","context":"ui","keys":["F2"],
            "uiAction":{"command":"settings.page7","argument":"keys"}}]})";
        const auto parsed = px::sdk::LoadCanonicalGameCatalogResources(source);
        suite.Require(parsed.Valid(), "UI shortcut catalog parses");
        suite.Expect(parsed.document.inputActions.size() == 1 &&
                         parsed.document.inputActions[0].uiCommand == "settings.page7" &&
                         parsed.document.inputActions[0].uiArgument == "keys",
                     "UI command and argument survive native loading");
        px::vn::GameCatalog runtime;
        suite.Require(runtime.LoadCanonical(source,
            R"({"format":"PrismatiXProject","schemaRevision":1,"assets":[]})",
            [](std::string_view) { return false; }).IsOk(),
            "runtime loads UI shortcuts");
        suite.Expect(runtime.InputActions().size() == 1 &&
                         runtime.InputActions()[0].uiCommand == "settings.page7",
                     "runtime catalog retains UI shortcut command");
        auto ambiguous = nlohmann::json::parse(source);
        ambiguous["inputActions"][0]["context"] = "game";
        ambiguous["inputActions"][0]["route"] = "settings";
        suite.Expect(HasCode(px::sdk::LoadCanonicalGameCatalogResources(ambiguous.dump()),
                             "PXCAT1123"),
                     "one action cannot both open a route and dispatch a UI command");
        auto misspelled = nlohmann::json::parse(source);
        misspelled["inputActions"][0]["uiAction"]["commmand"] = "settings.page7";
        suite.Expect(HasCode(px::sdk::LoadCanonicalGameCatalogResources(misspelled.dump()),
                             "PXCAT1123"),
                     "misspelled UI action fields fail at native loading");
    });

    return suite.Finish();
}
