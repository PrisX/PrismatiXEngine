#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <SDL3/SDL.h>

#include "Engine/Core/Uuid.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/IO/Archive.h"
#include "Engine/IO/Crypto.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/Runtime.h"
#include "Engine/SDK/Packager.h"
#include "Engine/Package/PackageManifest.h"
#include "Engine/SDK/CharacterResources.h"
#include "Engine/SDK/GameCatalogResources.h"
#include "Engine/SDK/RuntimeIr.h"
#include "Engine/Session/RuntimeSession.h"
#include "Engine/UI/UiApplication.h"
#include "Engine/UI/UIContext.h"
#include "Engine/UI/Widgets.h"
#include "Tests/TestSupport/TestHarness.h"
#include "Tests/TestSupport/CanonicalUiFixture.h"

namespace {

void Write(const std::filesystem::path& path, const std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

px::sdk::PackageInput Input(const std::filesystem::path& root, const std::string& uri) {
    const auto path = root / std::filesystem::path(uri);
    return { uri, px::sdk::ComputePackageFingerprint(path), std::filesystem::file_size(path) };
}

bool HasDiagnostic(const px::sdk::PackageRunResult& result,
                   const std::string_view code) {
    return std::any_of(
        result.diagnostics.begin(), result.diagnostics.end(),
        [code](const auto& diagnostic) { return diagnostic.code == code; });
}

std::uint64_t SurfaceHash(const SDL_Surface* surface) {
    if (!surface || !surface->pixels || surface->pitch <= 0 || surface->h <= 0)
        return 0;
    const auto* bytes = static_cast<const std::uint8_t*>(surface->pixels);
    const std::size_t count = static_cast<std::size_t>(surface->pitch) *
                              static_cast<std::size_t>(surface->h);
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t index = 0; index < count; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string UiTitle() {
    return px::test::CanonicalUiFixtureText(R"({
      "format":"PrismatiXUIScene","schemaRevision":1,
      "id":"4ff53084-a967-4ff1-88e5-266d083987b4","revision":4,
      "name":"Title","width":1280,"height":720,
      "rootId":"11111111-1111-4111-8111-111111111111",
      "nodes":[
        {"id":"11111111-1111-4111-8111-111111111111","parentId":null,"order":0,
         "kind":"control","name":"Root","visible":true,"locked":false,
         "layout":{"mode":"free","x":0,"y":0,"width":1280,"height":720,"anchorX":0,"anchorY":0,"pivotX":0,"pivotY":0,"margin":0,"alignment":"start","sizeRule":"fixed"},
         "content":{"text":"","assetId":null},
         "appearance":{"backgroundColor":"#16121A","textColor":"#FFFFFF","opacity":1,"styleToken":null,"hoverBackgroundColor":null,"focusColor":null,"disabledOpacity":0.5},
         "interaction":{"onClick":null},"accessibility":{"label":"","role":"presentation"}},
        {"id":"22222222-2222-4222-8222-222222222222","parentId":"11111111-1111-4111-8111-111111111111","order":0,
         "kind":"button","name":"Start","visible":true,"locked":false,
         "layout":{"mode":"free","x":-120,"y":-30,"width":240,"height":60,"anchorX":0.5,"anchorY":0.5,"pivotX":0.5,"pivotY":0.5,"margin":0,"alignment":"center","sizeRule":"fixed"},
         "content":{"text":"Start","assetId":null},
         "appearance":{"backgroundColor":"#F052A0","textColor":"#FFFFFF","opacity":1,"styleToken":null,"hoverBackgroundColor":"#FF79BA","focusColor":"#FFFFFF","disabledOpacity":0.5},
         "interaction":{"onClick":{"id":"game.start","arguments":{}}},
         "accessibility":{"label":"Start game","role":"button"}}
      ],"theme":[]
    })");
}

std::string RuntimeGameCatalog() {
    return R"({
      "format":"PrismatiXGame","schemaRevision":2,
      "variables":[{"name":"affinity","type":"integer","default":7,"scope":"profile"}],
      "inputBindings":[{"key":"Escape","command":"screen.open","argument":"settings"}],
      "gallery":[{"id":"ending-rin","title":"Rin Ending",
        "image":{"id":"33333333-3333-4333-8333-333333333333","path":"Assets/rin.png"},
        "thumbnail":{"id":"33333333-3333-4333-8333-333333333333","path":"Assets/rin.png"}}],
      "unlockables":[]
    })";
}

px::sdk::PackageRequest FixtureRequest(const std::filesystem::path& root, const std::filesystem::path& output) {
    px::sdk::PackageRequest request;
    request.requestId = "package-contract";
    request.gameId = "package-contract-game";
    request.projectRoot = root;
    request.outputDir = output;
#ifdef _WIN32
    request.playerExecutable = root / "Tools/PrismatiXPlayer.exe";
#else
    request.playerExecutable = root / "Tools/PrismatiXPlayer";
#endif
    request.title = "雪の物語";
    request.width = 1280;
    request.height = 720;
    request.startScript = "Content/Runtime/start.pxir";
    request.sourceMap = "Content/Runtime/start.pxmap";
    request.extensions = {};
    request.startRoute = "title";
    request.routes = { { "title", "Content/UI/Title.pxui" } };
    request.contentVersion = "fixture-v1";
    request.saveVersion = 1;
    request.encryption = true;
    request.compression = px::sdk::PackageCompression::Maximum;
    request.inputs = {
        Input(root, "Assets/rin.png"),
        Input(root, "Characters/11111111-1111-4111-8111-111111111111.pxcharacter"),
        Input(root, "Content/Localization/ja-JP.json"),
        Input(root, "Content/game.pxgame"),
        Input(root, "Content/UI/Title.pxui"),
        Input(root, "Content/Runtime/start.pxir"),
        Input(root, "Content/Runtime/start.pxmap"),
        Input(root, "Runtime/Locales/ja-JP/main.pxir"),
        Input(root, "Runtime/Locales/ja-JP/main.pxmap"),
        Input(root, "Story/Entry.pxstory"),
        Input(root, "Story/story.pxindex"),
        Input(root, "project.pxproject"),
    };
    request.cancelFile = root / ".cancel-package";
    return request;
}

}  // namespace

int main() {
    px::test::Suite suite("PackagerContract");
    px::test::TempDirectory fixture("packager-contract");
    const auto root = fixture.path / "Project";
    const auto output = fixture.path / "Build/Game";

    Write(root / "Content/Runtime/start.pxir", R"({
      "format":"PrismatiXRuntimeIR",
      "schemaRevision":2,
      "documentId":"99999999-9999-4999-8999-999999999999",
      "committedRevision":4,
      "operations":[{
        "operationId":"line-1",
        "sourceId":"line-1",
        "sourceLine":1,
        "kind":"dialogue",
        "text":"hello",
        "arguments":{"speaker":"雪","text":"hello"}
      }]
    })");
    Write(root / "Content/Runtime/start.pxmap", R"({
      "format":"PrismatiXSourceMap","schemaRevision":2,
      "documentId":"99999999-9999-4999-8999-999999999999",
      "mappings":[{"operationId":"line-1","sourceId":"line-1",
        "sourceUri":"Story/Entry.pxstory","startLine":1,"startColumn":1,
        "endLine":1,"endColumn":5}]
    })");
    Write(root / "Runtime/Locales/ja-JP/main.pxir",
          Read(root / "Content/Runtime/start.pxir"));
    Write(root / "Runtime/Locales/ja-JP/main.pxmap",
          Read(root / "Content/Runtime/start.pxmap"));
    Write(root / "Content/UI/Title.pxui", UiTitle());
    Write(root / "Content/game.pxgame", RuntimeGameCatalog());
    Write(root / "Content/Localization/ja-JP.json", R"({
      "format":"PrismatiXLocale","schemaRevision":2,
      "locale":"ja-JP","strings":{}
    })");
    Write(root / "Story/Entry.pxstory", "雪: hello\n[end]\n");
    Write(root / "Story/story.pxindex", R"({
      "format":"PrismatiXStoryIndex","schemaRevision":2,"id":"main",
      "entryScene":"entry-scene",
      "chapters":[{"id":"entry","title":"Entry","scenes":["entry-scene"]}],
      "scenes":[{"id":"entry-scene","sources":{"ja-JP":"Story/Entry.pxstory"}}]
    })");
    Write(root / "Assets/rin.png", "fixture-image");
    Write(root / "Characters/11111111-1111-4111-8111-111111111111.pxcharacter",
          R"({
      "format":"PrismatiXCharacter","schemaRevision":2,
      "id":"11111111-1111-4111-8111-111111111111","revision":0,
      "displayName":"凛","aliases":["rin"],
      "voiceDirectory":"Content/Audio/Voice/Rin",
      "defaultExpressionId":"22222222-2222-4222-8222-222222222222",
      "expressions":[{
        "id":"22222222-2222-4222-8222-222222222222","name":"Neutral",
        "aliases":["neutral"],
        "assetId":"33333333-3333-4333-8333-333333333333",
        "thumbnailAssetId":null
      }]
    })");
    Write(root / "project.pxproject",
          R"({
      "format":"PrismatiXProject","schemaRevision":2,
      "id":"package-contract-game","name":"雪の物語","version":"0.2.0",
      "contentVersion":"fixture-v1","saveVersion":1,
      "resolution":{"width":1280,"height":720},
      "entry":{"story":"entry-scene","ui":"title"},
      "defaultLocale":"ja-JP","supportedLocales":["ja-JP"],
      "storyIndex":"Story/story.pxindex","gameCatalog":"Content/game.pxgame",
      "extensions":[],"uiEntryPoints":{"title":"Content/UI/Title.pxui"},
      "assets":[{
        "id":"33333333-3333-4333-8333-333333333333",
        "name":"Rin","kind":"character","source":"Assets/rin.png",
        "sourceFileName":"rin.png","tags":[],"size":13,"fingerprint":"fixture"
      }],
      "characters":[{
        "id":"11111111-1111-4111-8111-111111111111",
        "displayName":"凛",
        "source":"Characters/11111111-1111-4111-8111-111111111111.pxcharacter"
      }]
    })");
#ifdef _WIN32
    Write(root / "Tools/PrismatiXPlayer.exe", "fake-player");
    Write(root / "Tools/runtime-fixture.dll", "fake-runtime");
#elif defined(__APPLE__)
    Write(root / "Tools/PrismatiXPlayer", "fake-player");
    Write(root / "Tools/libruntime-fixture.dylib", "fake-runtime");
#else
    Write(root / "Tools/PrismatiXPlayer", "fake-player");
    Write(root / "Tools/libruntime-fixture.so", "fake-runtime");
#endif
    Write(output / "previous-success.txt", "keep-on-failure");

    auto request = FixtureRequest(root, output);
    std::vector<px::sdk::PackageEvent> events;
    suite.Run("StrictRequest_ParsesLockedWireContract", [&] {
        std::ostringstream json;
        json << R"({"format":"PrismatiXPackageRequest","schemaRevision":2,)"
             << R"("requestId":"parse-contract",)"
             << R"("gameId":"parse-contract-game",)"
             << R"("projectRoot":")" << root.generic_string() << R"(",)"
             << R"("outputDir":")" << output.generic_string() << R"(",)"
             << R"("playerExecutable":")" << request.playerExecutable.generic_string() << R"(",)"
             << R"("title":"Contract","width":1280,"height":720,)"
             << R"("startScript":"Content/Runtime/start.pxir",)"
             << R"("sourceMap":"Content/Runtime/start.pxmap",)"
             << R"("extensions":[],)"
             << R"("graphicsTier":"basic",)"
             << R"("startRoute":"title",)"
             << R"("routes":[{"id":"title","scene":"Content/UI/Title.pxui"}],)"
             << R"("contentVersion":"v1","saveVersion":1,"encryption":false,)"
             << R"("compression":"balanced","inputs":[)"
             << R"({"uri":"Content/Runtime/start.pxir","fingerprint":")" << request.inputs[5].fingerprint << R"(","size":)" << request.inputs[5].size << R"(}],)"
             << R"("cancelFile":")" << request.cancelFile.generic_string() << R"("})";
        const auto parsed = px::sdk::ParsePackageRequest(json.str());
        suite.Expect(parsed.Valid(), "locked package request parses");
        suite.Expect(parsed.request.routes.size() == 1 && parsed.request.routes.front().id == "title", "typed Player route is retained by the SDK contract");

        px::sdk::PackageEvent progress;
        progress.kind = px::sdk::PackageEventKind::Progress;
        progress.requestId = "parse-contract";
        progress.phase = "archive";
        progress.current = 1;
        progress.total = 2;
        progress.message = "Reading input";
        const auto wire = px::sdk::SerializePackageEvent(progress);
        suite.Expect(wire.find("\"event\":\"progress\"") != std::string::npos && wire.find("\"phase\":\"archive\"") != std::string::npos && wire.find("\"protocolVersion\":1") != std::string::npos, "progress event uses the locked line protocol", wire);

        px::sdk::PackageDiagnostic first{"PXPKGTEST1001", "First failure", false};
        first.documentId = "ui-document";
        first.sourceId = "component-property";
        first.span = px::sdk::PackageDiagnostic::SourceSpan{
            .path = "Content/UI/Panel.pxuicomponent",
            .start = {.line = 12, .column = 3, .offset = 100},
            .end = {.line = 12, .column = 20, .offset = 117}};
        first.hint = "Repair the exposed path";
        first.cause = "missing path";
        px::sdk::PackageEvent failed;
        failed.kind = px::sdk::PackageEventKind::Failed;
        failed.requestId = "parse-contract";
        failed.code = first.code;
        failed.message = first.message;
        failed.diagnostics = {first,
                              {"PXPKGTEST1002", "Second failure", true}};
        const auto failedWire = nlohmann::json::parse(
            px::sdk::SerializePackageEvent(failed));
        suite.Expect(
            failedWire["severity"] == "error" &&
                failedWire["diagnostic"]["code"] == "PXPKGTEST1001" &&
                failedWire["diagnostic"]["documentId"] == "ui-document" &&
                failedWire["diagnostic"]["sourceId"] ==
                    "component-property" &&
                failedWire["diagnostic"]["span"]["path"] ==
                    "Content/UI/Panel.pxuicomponent" &&
                failedWire["diagnostic"]["span"]["start"]["line"] == 12 &&
                failedWire["diagnostic"]["hint"] ==
                    "Repair the exposed path" &&
                failedWire["diagnostic"]["cause"] == "missing path" &&
                failedWire["diagnostics"].size() == 2,
            "failed package events retain the canonical structured diagnostic set",
            failedWire.dump());
    });

    suite.Run("Package_CreatesPlayerCompatibleAtomicDistribution", [&] {
        const auto result = px::sdk::RunPackager(request, [&](const auto& event) { events.push_back(event); });
        suite.Require(result.Completed(), "SDK Packager completes");
        suite.Expect(std::filesystem::is_regular_file(output / "Content.pdx"), "Content.pdx is promoted");
        suite.Expect(std::filesystem::is_regular_file(output / "Package/manifest.json"), "explicit package manifest is promoted");
        suite.Expect(std::filesystem::is_regular_file(output / request.playerExecutable.filename()), "Player is copied");
#ifdef _WIN32
        suite.Expect(std::filesystem::is_regular_file(output / "runtime-fixture.dll"), "adjacent Windows runtime library is copied");
#elif defined(__APPLE__)
        suite.Expect(std::filesystem::is_regular_file(output /
                                                      "libruntime-fixture.dylib"),
                     "adjacent macOS runtime library is copied");
#else
        suite.Expect(std::filesystem::is_regular_file(output /
                                                      "libruntime-fixture.so"),
                     "adjacent runtime library is copied");
#endif
        suite.Expect(!std::filesystem::exists(output / "previous-success.txt"), "successful promotion replaces the previous output");
        suite.Expect(!events.empty() && events.back().kind == px::sdk::PackageEventKind::Completed && events.back().inputCount == request.inputs.size(), "completion event reports the promoted artifact");

        const auto manifest = px::sdk::detail::ParsePackageManifest(
            Read(output / "Package/manifest.json"));
        suite.Require(manifest.Valid(), "Player parses the generated package manifest");
        suite.Expect(
            manifest.manifest.engineVersion == "0.2.0" &&
                manifest.manifest.startRuntimeIr == request.startScript &&
                manifest.manifest.startRoute == "title" &&
                manifest.manifest.gameId == request.gameId &&
                !manifest.manifest.packageFingerprint.empty(),
            "package manifest retains entrypoint and stable save identity"
        );
        const auto* sceneReference = manifest.manifest.routes.size() == 1
                                         ? &manifest.manifest.routes.front()
                                         : nullptr;
        suite.Expect(sceneReference && sceneReference->sceneId == "4ff53084-a967-4ff1-88e5-266d083987b4" && sceneReference->scene == "Content/UI/Title.pxui", "startRoute maps to the packaged UI document identity");

        px::io::Archive archive;
        const auto key = px::crypto::DeriveKey(manifest.manifest.archiveKey);
        suite.Require(archive.Open((output / "Content.pdx").string(), &key), "Player archive reader opens generated PDX4");
        suite.Expect(archive.Contains(request.startScript) && archive.Contains("Content/UI/Title.pxui"), "compiled entrypoint and route scene are in the archive");
        const auto packagedEntryIr = archive.Read(request.startScript);
        suite.Require(packagedEntryIr.has_value(),
                      "Player can read the packaged narrative entry Runtime IR");
        const auto parsedEntryIr = px::sdk::ParseRuntimeIr(std::string(
            packagedEntryIr->begin(), packagedEntryIr->end()));
        suite.Expect(
            parsedEntryIr.Valid() &&
                parsedEntryIr.document.documentId ==
                    "99999999-9999-4999-8999-999999999999",
            "packaged Player startScript resolves to the project entry chapter Scene");
        px::io::VFS playerVfs;
        suite.Require(
            playerVfs.MountArchive((output / "Content.pdx").string(), &key),
            "production Player VFS mounts the generated package");
        px::audio::AudioEngine playerAudio(playerVfs);
        px::graphics::AssetCache playerAssets(nullptr, playerVfs);
        px::graphics::Renderer2D playerRenderer(nullptr, playerAssets);
        px::RuntimeSession playerSession(
            {.vfs = playerVfs,
             .audio = playerAudio,
             .renderer = playerRenderer,
             .assets = playerAssets});
        suite.Require(
            playerSession.StartRuntimeIr(request.startScript),
            "packaged Player entry loads through the production RuntimeSession path");
        suite.Expect(
            playerSession.VM().CurrentScript() == request.startScript &&
                playerSession.VM().CurrentProgram().code.size() == 1 &&
                playerSession.Dialogue().State().speaker == "雪" &&
                playerSession.Dialogue().State().fullText == "hello",
            "production Player starts the selected entry Scene semantics from Content.pdx");
        suite.Expect(
            !playerSession.StartRuntimeIr("Content/Runtime/missing.pxir") &&
                playerSession.LastStartDiagnostics().size() == 1 &&
                playerSession.LastStartDiagnostics().front().code ==
                    "PXRUNTIME7310",
            "missing packaged entry fails with a stable RuntimeSession diagnostic");
        suite.Expect(
            archive.Entries().size() == 12 &&
                archive.Entries()[0].name == "Assets/rin.png" &&
                archive.Entries()[1].name ==
                    "Characters/11111111-1111-4111-8111-111111111111.pxcharacter" &&
                archive.Entries()[2].name == "Content/Localization/ja-JP.json" &&
                archive.Entries()[3].name == "Content/Runtime/start.pxir" &&
                archive.Entries()[4].name == "Content/Runtime/start.pxmap" &&
                archive.Entries()[5].name == "Content/UI/Title.pxui" &&
                archive.Entries()[6].name == "Content/game.pxgame" &&
                archive.Entries()[7].name ==
                    "Runtime/Locales/ja-JP/main.pxir" &&
                archive.Entries()[8].name ==
                    "Runtime/Locales/ja-JP/main.pxmap" &&
                archive.Entries()[9].name == "Story/Entry.pxstory" &&
                archive.Entries()[10].name == "Story/story.pxindex" &&
                archive.Entries()[11].name == "project.pxproject",
            "archive input ordering is deterministic");
        const auto packagedProjectManifest = archive.Read("project.pxproject");
        suite.Require(packagedProjectManifest.has_value(),
                      "Player can read the packaged project manifest");
        const auto packagedCharacters = px::sdk::LoadCharacterResources(
            std::string(packagedProjectManifest->begin(),
                        packagedProjectManifest->end()),
            [&archive](const std::string_view uri)
                -> std::optional<std::string> {
                const auto bytes = archive.Read(std::string(uri));
                return bytes ? std::optional<std::string>{
                                   std::string(bytes->begin(), bytes->end())}
                             : std::nullopt;
            },
            [&archive](const std::string_view uri) {
                return archive.Contains(std::string(uri));
            });
        suite.Require(packagedCharacters.Valid(),
                      "packaged Player reads characterResources through the shared loader");
        suite.Expect(
                packagedCharacters.document.characters.size() == 1 &&
                packagedCharacters.document.characters.front().displayName ==
                    "凛" &&
                packagedCharacters.document.characters.front().aliases ==
                    std::vector<std::string>{"rin"} &&
                packagedCharacters.document.characters.front()
                        .defaultExpressionId ==
                    "22222222-2222-4222-8222-222222222222",
            "packaged Player receives the same character runtime semantics");
        const auto packagedCatalog = archive.Read("Content/game.pxgame");
        suite.Require(packagedCatalog.has_value(),
                      "Player can read the packaged runtime GameCatalog");
        const auto catalog = px::sdk::LoadCanonicalGameCatalogResources(
            std::string(packagedCatalog->begin(), packagedCatalog->end()));
        suite.Expect(
            catalog.Valid() && catalog.document.variables.size() == 1 &&
                catalog.document.variables.front().defaultValue == 7 &&
                catalog.document.inputBindings.size() == 1 &&
                catalog.document.gallery.size() == 1,
            "Packager and Player share canonical GameCatalog semantics");
        const auto resolvedCatalog =
            px::sdk::ResolveGameCatalogGalleryResources(
                catalog.document,
                std::string(packagedProjectManifest->begin(),
                            packagedProjectManifest->end()),
                [&archive](const std::string_view uri) {
                    return archive.Contains(std::string(uri));
                });
        suite.Expect(
            resolvedCatalog.Valid() &&
                resolvedCatalog.document.gallery.front().image.assetId ==
                    "33333333-3333-4333-8333-333333333333" &&
                resolvedCatalog.document.gallery.front().image.assetPath ==
                    "Assets/rin.png",
            "packaged Player resolves Gallery UUIDs through the packaged manifest");

        const auto packagedScene = archive.Read(sceneReference->scene);
        suite.Require(packagedScene.has_value(),
                      "Player can read startRoute UI document from Content.pdx");
        px::ui::UIContext playerContext;
        std::size_t startActions = 0;
        suite.Require(
            playerContext.Commands()
                .Register("game.start", [&startActions](const px::Variant&) {
                    ++startActions;
                    return px::Status::Ok();
                })
                .IsOk(),
            "headless Player Action route registers");
        px::ui::UiApplication playerUi(playerContext);
        const std::string packagedText(packagedScene->begin(), packagedScene->end());
        const auto booted = playerUi.ApplyText(
            packagedText,
            {.sourcePath = sceneReference->scene});
        suite.Require(static_cast<bool>(booted),
                      "Packager output boots through the shared Player UI document path");
        const auto startId =
            px::Uuid::Parse("22222222-2222-4222-8222-222222222222");
        auto* start = startId
                          ? dynamic_cast<px::ui::Button*>(
                                playerContext.Root()->Find(*startId))
                          : nullptr;
        suite.Require(start != nullptr,
                      "headless Player boot creates the authored start button");
        start->Activate();
        suite.Expect(startActions == 1,
                     "packaged UI document dispatches its authored Player Action");
    });

    suite.Run("LocaleFontChain_IsPackagedAndPreflightChecksGlyphCoverage", [&] {
        const auto localePath = root / "Content/Localization/ja-JP.json";
        const auto originalLocale = Read(localePath);

        Write(localePath, R"({
          "format":"PrismatiXLocale","schemaRevision":2,
          "locale":"ja-JP","fontChain":["Content/Fonts/missing.ttf"],
          "strings":{"snow":"雪"}
        })");
        auto missing = FixtureRequest(
            root, fixture.path / "Build/MissingLocaleFont");
        missing.requestId = "missing-locale-font";
        const auto missingResult = px::sdk::RunPackager(missing);
        suite.Expect(!missingResult.Completed() &&
                         HasDiagnostic(missingResult, "PXPKG1265"),
                     "declared locale fonts must be present in package inputs");

        const auto fontPath = root / "Content/Fonts/NotoSansTC-Bold.ttf";
        std::filesystem::create_directories(fontPath.parent_path());
        std::filesystem::copy_file(
            std::filesystem::path(PRISMATIX_RESOURCE_ROOT) /
                "Fonts/NotoSansTC-Bold.ttf",
            fontPath, std::filesystem::copy_options::overwrite_existing);
        Write(localePath, R"({
          "format":"PrismatiXLocale","schemaRevision":2,
          "locale":"ja-JP","fontChain":["Content/Fonts/NotoSansTC-Bold.ttf"],
          "strings":{"snow":"雪"}
        })");
        auto covered = FixtureRequest(
            root, fixture.path / "Build/CoveredLocaleFont");
        covered.requestId = "covered-locale-font";
        covered.encryption = false;
        covered.compression = px::sdk::PackageCompression::None;
        for (auto& input : covered.inputs) input = Input(root, input.uri);
        covered.inputs.push_back(
            Input(root, "Content/Fonts/NotoSansTC-Bold.ttf"));
        const auto coveredResult = px::sdk::RunPackager(covered);
        suite.Expect(coveredResult.Completed(),
                     "a fallback chain covering locale and Story glyphs packages successfully");

        Write(localePath, R"({
          "format":"PrismatiXLocale","schemaRevision":2,
          "locale":"ja-JP","fontChain":["Content/Fonts/NotoSansTC-Bold.ttf"],
          "strings":{"unsupported":"\uDBFF\uDFFF"}
        })");
        auto uncovered = covered;
        uncovered.requestId = "uncovered-locale-glyph";
        uncovered.outputDir = fixture.path / "Build/UncoveredLocaleGlyph";
        for (auto& input : uncovered.inputs) input = Input(root, input.uri);
        const auto uncoveredResult = px::sdk::RunPackager(uncovered);
        suite.Expect(!uncoveredResult.Completed() &&
                         HasDiagnostic(uncoveredResult, "PXPKG1268") &&
                         !std::filesystem::exists(uncovered.outputDir /
                                                  "Content.pdx"),
                     "missing glyph coverage fails before output promotion");
        Write(localePath, originalLocale);
    });

    suite.Run("CustomEffect_IsCompiledReflectedAndShipsWithoutSource", [&] {
        const auto effectRoot = fixture.path / "EffectProject";
        const auto effectOutput = fixture.path / "Build/EffectGame";
        std::filesystem::copy(root, effectRoot,
                              std::filesystem::copy_options::recursive);
        auto project = nlohmann::json::parse(Read(effectRoot / "project.pxproject"));
        project["graphicsTier"] = "gpu-effects";
        project["effects"] = nlohmann::json::array(
            {{{"id", "dream-tone"},
              {"source", "Effects/dream-tone.pxeffect"}}});
        Write(effectRoot / "project.pxproject", project.dump());
        Write(effectRoot / "Effects/dream-tone.pxeffect", R"({
          "format":"PrismatiXEffect","schemaRevision":2,
          "id":"dream-tone","targetLayer":"stage",
          "shader":"Effects/dream-tone.frag.hlsl",
          "uniforms":[{"name":"amount","type":"number","slot":0,
            "default":0.5,"minimum":0,"maximum":1}]
        })");
        Write(effectRoot / "Effects/dream-tone.frag.hlsl", R"(
          cbuffer PrismatiXEffectContext : register(b0, space3) {
            float2 texelSize; float progress; float randomSeed;
            float4 parameters[8];
          };
          Texture2D stageTexture : register(t0, space2);
          SamplerState stageSampler : register(s0, space2);
          struct Input { float4 color : COLOR0; float2 uv : TEXCOORD0; };
          float4 main(Input input) : SV_Target {
            float4 color = stageTexture.Sample(stageSampler, input.uv) * input.color;
            return lerp(color, float4(color.b, color.r, color.g, color.a),
                        saturate(parameters[0].x * progress));
          }
        )");
        auto effectRequest = FixtureRequest(effectRoot, effectOutput);
        effectRequest.requestId = "package-custom-effect";
        effectRequest.graphicsTier = "gpu-effects";
        for (auto& input : effectRequest.inputs)
            input = Input(effectRoot, input.uri);
        effectRequest.inputs.push_back(
            Input(effectRoot, "Effects/dream-tone.pxeffect"));
        effectRequest.inputs.push_back(
            Input(effectRoot, "Effects/dream-tone.frag.hlsl"));

        const auto result = px::sdk::RunPackager(effectRequest);
        suite.Require(result.Completed(),
                      "valid custom HLSL compiles during packaging");
        const auto manifest = px::sdk::detail::ParsePackageManifest(
            Read(effectOutput / "Package/manifest.json"));
        suite.Require(manifest.Valid() &&
                          manifest.manifest.customEffects.size() == 1,
                      "Player parses reflected custom effect metadata");
        const auto& effect = manifest.manifest.customEffects.front();
        suite.Expect(effect.id == "dream-tone" && effect.targetLayer == "stage" &&
                         effect.samplerCount == 1 &&
                         effect.uniformBufferCount == 1 &&
                         effect.artifacts.size() == 3 &&
                         effect.uniforms.size() == 1,
                     "custom effect manifest has a fixed bounded resource schema");

        px::io::Archive archive;
        const auto key = px::crypto::DeriveKey(manifest.manifest.archiveKey);
        suite.Require(archive.Open((effectOutput / "Content.pdx").string(), &key),
                      "custom-effect package archive opens");
        suite.Expect(archive.Contains("Shaders/dream-tone/fragment.spv") &&
                         archive.Contains("Shaders/dream-tone/fragment.dxil") &&
                         archive.Contains("Shaders/dream-tone/fragment.msl"),
                     "all release shader formats are packaged");
        suite.Expect(!archive.Contains("Effects/dream-tone.frag.hlsl"),
                     "Player package does not contain injectable shader source");
        suite.Expect(
            !std::filesystem::exists(effectOutput / "SDL3_shadercross.dll") &&
                !std::filesystem::exists(effectOutput / "dxcompiler.dll"),
            "Player distribution excludes build-time shader compiler libraries");
        for (const auto& artifact : effect.artifacts) {
            const auto bytes = archive.Read(artifact.asset);
            suite.Expect(bytes && !bytes->empty() &&
                             artifact.fingerprint.size() == 64,
                         "manifest references a non-empty fingerprinted artifact");
        }

        px::RuntimeConfig runtimeConfig;
        runtimeConfig.title = "PrismatiX packaged custom effect conformance";
        runtimeConfig.width = runtimeConfig.logicalWidth = 320;
        runtimeConfig.height = runtimeConfig.logicalHeight = 180;
        runtimeConfig.mountDirs.clear();
        runtimeConfig.mountArchives = {
            (effectOutput / "Content.pdx").string()};
        runtimeConfig.archiveKey = manifest.manifest.archiveKey;
        runtimeConfig.graphicsTier = "gpu-effects";
        px::graphics::CustomEffectDescriptor runtimeEffect;
        runtimeEffect.id = effect.id;
        runtimeEffect.targetLayer = effect.targetLayer;
        runtimeEffect.samplerCount = effect.samplerCount;
        runtimeEffect.uniformBufferCount = effect.uniformBufferCount;
        for (const auto& uniform : effect.uniforms)
            runtimeEffect.uniforms.push_back(
                {uniform.name, uniform.type, uniform.slot,
                 uniform.defaultValue, uniform.minimum, uniform.maximum});
        for (const auto& artifact : effect.artifacts)
            runtimeEffect.artifacts.push_back(
                {artifact.format, artifact.asset, artifact.fingerprint});
        runtimeConfig.customEffects.push_back(std::move(runtimeEffect));

        px::Runtime runtime;
        suite.Require(runtime.Init(runtimeConfig),
                      "Runtime loads the packaged device-specific custom shader artifact");
        suite.Require(runtime.Renderer().HasCustomEffect("dream-tone"),
                      "custom effect is registered in the production compositor");
        px::RuntimeSession session({runtime.VFS(), runtime.Audio(),
                                    runtime.Renderer(), runtime.Assets()});
        suite.Expect(std::ranges::any_of(
                         session.Timeline().RegisteredClips(),
                         [](const auto& entry) {
                             return entry.second.name == "Screen/dream-tone";
                         }),
                     "packaged custom effect is reachable through screen_effect/Timeline");

        const auto render = [&](const px::graphics::StagePostEffects& effects) {
            runtime.GetWindow().Clear({0, 0, 0, 255});
            suite.Require(runtime.Renderer().BeginStageLayer(),
                          "custom effect Stage target begins");
            runtime.Renderer().DrawRect({0, 0, 320, 180},
                                        {225, 225, 225, 255});
            runtime.Renderer().DrawRect({80, 35, 160, 110},
                                        {220, 35, 70, 255});
            runtime.Renderer().EndStageLayer(effects);
            SDL_Surface* pixels = SDL_RenderReadPixels(
                runtime.Renderer().Handle(), nullptr);
            const auto hash = SurfaceHash(pixels);
            SDL_DestroySurface(pixels);
            return hash;
        };
        const std::uint64_t baseline = render({});
        px::graphics::StagePostEffects custom;
        custom.customEffect = "dream-tone";
        custom.customProgress = 1.0f;
        const auto defaults =
            runtime.Renderer().CustomEffectDefaults("dream-tone");
        suite.Require(defaults.has_value(),
                      "custom effect default uniforms are available");
        custom.customParameters = *defaults;
        const std::uint64_t effected = render(custom);
        suite.Expect(baseline != 0 && effected != 0 && baseline != effected,
                     "packaged custom effect visibly changes the Stage framebuffer");
        runtime.Shutdown();

        Write(effectRoot / "Effects/dream-tone.frag.hlsl", R"(
          Texture2D stageTexture : register(t0);
          SamplerState stageSampler : register(s0);
          float4 main(float2 uv : TEXCOORD0) : SV_Target {
            return stageTexture.Sample(stageSampler, uv);
          }
        )");
        effectRequest.requestId = "reject-custom-effect-bindings";
        effectRequest.outputDir = fixture.path / "Build/InvalidEffectBindings";
        for (auto& input : effectRequest.inputs)
            input = Input(effectRoot, input.uri);
        const auto invalidBindings = px::sdk::RunPackager(effectRequest);
        suite.Expect(!invalidBindings.Completed() &&
                         HasDiagnostic(invalidBindings, "PXPKG1506") &&
                         !std::filesystem::exists(effectRequest.outputDir /
                                                  "Content.pdx"),
                     "custom shaders without fixed reflected bindings fail before output promotion");
    });

    suite.Run("Cancellation_PreservesPriorSuccessfulOutput", [&] {
        const auto cancelledOutput = fixture.path / "Build/Cancelled";
        Write(cancelledOutput / "previous-success.txt", "preserved");
        auto cancelled = FixtureRequest(root, cancelledOutput);
        cancelled.requestId = "cancel-contract";
        bool requested = false;
        const auto result = px::sdk::RunPackager(cancelled, [&](const auto& event) {
            if (!requested && event.kind == px::sdk::PackageEventKind::Progress && event.phase == "archive") {
                Write(cancelled.cancelFile, "cancel");
                requested = true;
            }
        });
        std::error_code cleanupError;
        std::filesystem::remove(cancelled.cancelFile, cleanupError);
        suite.Expect(result.exitCode == px::sdk::PackageExitCode::Cancelled, "cancel exits with the dedicated status");
        suite.Expect(Read(cancelledOutput / "previous-success.txt") == "preserved" && !std::filesystem::exists(cancelledOutput / "Content.pdx"), "cancel leaves prior successful output untouched");
    });

    suite.Run("ValidationFailure_PreservesPriorSuccessfulOutput", [&] {
        const auto failedOutput = fixture.path / "Build/Failed";
        Write(failedOutput / "previous-success.txt", "preserved");
        auto failed = FixtureRequest(root, failedOutput);
        failed.requestId = "failure-contract";
        failed.inputs.front().fingerprint = std::string(64, '0');
        const auto result = px::sdk::RunPackager(failed);
        suite.Expect(result.exitCode == px::sdk::PackageExitCode::Failed && !result.diagnostics.empty() && result.diagnostics.front().code == "PXPKG1216", "fingerprint drift is a typed failure");
        suite.Expect(Read(failedOutput / "previous-success.txt") == "preserved", "failure leaves prior successful output untouched");
    });

    suite.Run("InvalidRuntimeGameCatalog_IsRejectedBeforePackaging", [&] {
        const auto catalogPath = root / "Content/game.pxgame";
        Write(catalogPath, R"({
          "format":"PrismatiXGame","schemaRevision":2,
          "variables":[{"name":"affinity","type":"integer","default":"seven","scope":"session"}],
          "gallery":[],"unlockables":[]
        })" );
        auto invalid = FixtureRequest(
            root, fixture.path / "Build/InvalidRuntimeCatalog");
        invalid.requestId = "invalid-runtime-catalog";
        const auto result = px::sdk::RunPackager(invalid);
        Write(catalogPath, RuntimeGameCatalog());
        suite.Expect(
            result.exitCode == px::sdk::PackageExitCode::Failed &&
                HasDiagnostic(result, "PXCAT1107"),
            "Packager executes the shared canonical GameCatalog contract before staging");
    });

    suite.Run("GalleryPathsAndUnknownUuids_AreRejectedBeforePackaging", [&] {
        const auto catalogPath = root / "Content/game.pxgame";
        Write(catalogPath, R"({
          "format":"PrismatiXGame","schemaRevision":2,
          "variables":[],"unlockables":[],
          "gallery":[{"id":"legacy","image":"Assets/rin.png"}]
        })" );
        auto legacyPath = FixtureRequest(
            root, fixture.path / "Build/LegacyGalleryPath");
        legacyPath.requestId = "legacy-gallery-path";
        const auto legacyResult = px::sdk::RunPackager(legacyPath);
        suite.Expect(
            legacyResult.exitCode == px::sdk::PackageExitCode::Failed &&
                (HasDiagnostic(legacyResult, "PXCAT1108") ||
                 HasDiagnostic(legacyResult, "PXCAT1110")),
            "canonical packages cannot use path-authoritative Gallery items");

        Write(catalogPath, R"({
          "format":"PrismatiXGame","schemaRevision":2,
          "variables":[],"unlockables":[],
          "gallery":[{"id":"unknown","image":{"id":"99999999-9999-4999-8999-999999999999","path":"Assets/rin.png"}}]
        })" );
        auto unknownUuid = FixtureRequest(
            root, fixture.path / "Build/UnknownGalleryUuid");
        unknownUuid.requestId = "unknown-gallery-uuid";
        const auto unknownResult = px::sdk::RunPackager(unknownUuid);
        Write(catalogPath, RuntimeGameCatalog());
        suite.Expect(
            unknownResult.exitCode == px::sdk::PackageExitCode::Failed &&
                HasDiagnostic(unknownResult, "PXCAT1024"),
            "Packager resolves Gallery identity through project.pxproject before staging");
    });

    suite.Run("SameSizeMutationAfterValidation_IsRejected", [&] {
        const auto changedOutput = fixture.path / "Build/Changed";
        Write(changedOutput / "previous-success.txt", "preserved");
        auto changed = FixtureRequest(root, changedOutput);
        changed.requestId = "changed-contract";
        bool injected = false;
        const auto scriptPath = root / "Content/Runtime/start.pxir";
        const std::string original = Read(scriptPath);
        const auto result = px::sdk::RunPackager(changed, [&](const auto& event) {
            if (!injected && event.kind == px::sdk::PackageEventKind::Progress && event.phase == "archive" && event.current == 0) {
                std::string mutation = original;
                const auto location = mutation.find("\"hello\"");
                if (location != std::string::npos) mutation[location + 1] = 'j';
                Write(scriptPath, mutation);
                injected = true;
            }
        });
        Write(scriptPath, original);
        suite.Expect(result.exitCode == px::sdk::PackageExitCode::Failed && !result.diagnostics.empty() && result.diagnostics.front().code == "PXPKG1305", "same-size TOCTOU mutation is a retryable input-changed failure");
        suite.Expect(Read(changedOutput / "previous-success.txt") == "preserved", "TOCTOU failure leaves prior successful output untouched");
    });

    suite.Run("UnsafeOutputAncestor_IsRejected", [&] {
        auto unsafe = FixtureRequest(root, fixture.path);
        unsafe.requestId = "unsafe-output-contract";
        const auto result = px::sdk::RunPackager(unsafe);
        suite.Expect(result.exitCode == px::sdk::PackageExitCode::Failed && !result.diagnostics.empty() && result.diagnostics.front().code == "PXPKG1225", "output ancestor cannot replace the project or runtime tree");
    });

    suite.Run("MissingStartRouteMapping_IsRejected", [&] {
        auto missingRoute = FixtureRequest(root, fixture.path / "Build/MissingRoute");
        missingRoute.requestId = "missing-route-contract";
        missingRoute.routes.clear();
        const auto result = px::sdk::RunPackager(missingRoute);
        suite.Expect(result.exitCode == px::sdk::PackageExitCode::Failed && !result.diagnostics.empty() && result.diagnostics.front().code == "PXPKG1222", "non-empty startRoute cannot produce an unbootable manifest");
    });

    suite.Run("MalformedUiRoute_IsRejected", [&] {
        const auto scenePath = root / "Content/UI/Title.pxui";
        const std::string original = Read(scenePath);
        Write(scenePath, "{}");
        auto malformed =
            FixtureRequest(root, fixture.path / "Build/MalformedRoute");
        malformed.requestId = "malformed-route-contract";
        const auto result = px::sdk::RunPackager(malformed);
        Write(scenePath, original);
        suite.Expect(
            result.exitCode == px::sdk::PackageExitCode::Failed &&
                HasDiagnostic(result, "PXSDKUI1002") &&
                std::ranges::any_of(result.diagnostics, [](const auto& diagnostic) {
                    return diagnostic.code == "PXSDKUI1002" &&
                           diagnostic.span &&
                           diagnostic.span->path == "Content/UI/Title.pxui";
                }),
            "Packager validates UI document content instead of trusting the extension");
    });

    suite.Run("MissingCharacterInput_IsRejectedBeforePackaging", [&] {
        auto missing = FixtureRequest(
            root, fixture.path / "Build/MissingCharacter");
        missing.requestId = "missing-character-contract";
        std::erase_if(missing.inputs, [](const auto& input) {
            return input.uri.ends_with(".pxcharacter");
        });
        const auto result = px::sdk::RunPackager(missing);
        suite.Expect(
            result.exitCode == px::sdk::PackageExitCode::Failed &&
                HasDiagnostic(result, "PXCHAR1011"),
            "Packager surfaces the shared missing character diagnostic");
    });

    suite.Run("MalformedCharacterInput_IsRejectedBeforePackaging", [&] {
        const auto characterPath =
            root /
            "Characters/11111111-1111-4111-8111-111111111111.pxcharacter";
        const auto original = Read(characterPath);
        Write(characterPath, "{");
        auto malformed = FixtureRequest(
            root, fixture.path / "Build/MalformedCharacter");
        malformed.requestId = "malformed-character-contract";
        const auto result = px::sdk::RunPackager(malformed);
        Write(characterPath, original);
        suite.Expect(
            result.exitCode == px::sdk::PackageExitCode::Failed &&
                HasDiagnostic(result, "PXCHAR1013"),
            "Packager surfaces the shared malformed character diagnostic");
    });

    suite.Run("InvalidDefaultExpression_IsRejectedBeforePackaging", [&] {
        const auto characterPath =
            root /
            "Characters/11111111-1111-4111-8111-111111111111.pxcharacter";
        const auto original = Read(characterPath);
        auto invalid = original;
        const auto defaultId =
            invalid.find("22222222-2222-4222-8222-222222222222");
        suite.Require(defaultId != std::string::npos,
                      "fixture contains a default expression UUID");
        invalid.replace(defaultId, 36,
                        "44444444-4444-4444-8444-444444444444");
        Write(characterPath, invalid);
        auto requestWithInvalidDefault = FixtureRequest(
            root, fixture.path / "Build/InvalidDefault");
        requestWithInvalidDefault.requestId = "invalid-default-contract";
        const auto result =
            px::sdk::RunPackager(requestWithInvalidDefault);
        Write(characterPath, original);
        suite.Expect(
            result.exitCode == px::sdk::PackageExitCode::Failed &&
                HasDiagnostic(result, "PXCHAR1025"),
            "Packager surfaces the shared invalid default diagnostic");
    });

    suite.Run("MissingExpressionAssetInput_IsRejectedBeforePackaging", [&] {
        auto missingAsset = FixtureRequest(
            root, fixture.path / "Build/MissingCharacterAsset");
        missingAsset.requestId = "missing-character-asset-contract";
        std::erase_if(missingAsset.inputs, [](const auto& input) {
            return input.uri == "Assets/rin.png";
        });
        const auto result = px::sdk::RunPackager(missingAsset);
        suite.Expect(
            result.exitCode == px::sdk::PackageExitCode::Failed &&
                HasDiagnostic(result, "PXCHAR1021"),
            "Packager surfaces the shared missing expression asset diagnostic");
    });

    return suite.Finish();
}
