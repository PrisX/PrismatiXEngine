#include <string>

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

bool HasCode(const px::sdk::GameCatalogResourcesLoadResult& result, const std::string_view code) {
    for (const auto& diagnostic : result.diagnostics)
        if (diagnostic.code == code) return true;
    return false;
}

}  // namespace

int main() {
    px::test::Suite suite("GameCatalogResourcesContract");

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
        const auto status = catalog.LoadRuntimeResources(
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

    return suite.Finish();
}
