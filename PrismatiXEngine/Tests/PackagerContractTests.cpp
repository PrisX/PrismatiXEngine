#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#include <nlohmann/json.hpp>
#include <SDL3/SDL.h>

#include "Engine/Core/Uuid.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/IO/Archive.h"
#include "Engine/IO/Crypto.h"
#include "Engine/IO/OverlaySignature.h"
#include "Engine/IO/PackageVerifier.h"
#include "Engine/Progression/GameSettings.h"
#include "Engine/Progression/GlobalProfileStore.h"
#include "Engine/Progression/Persist.h"
#include "Engine/Progression/SaveSystem.h"
#include "Engine/Resources/TypedDocument.h"
#include "Engine/Runtime.h"
#include "Engine/SDK/Packager.h"
#include "Engine/Package/PackageManifest.h"
#include "Engine/SDK/CharacterResources.h"
#include "Engine/SDK/GameCatalogResources.h"
#include "Engine/SDK/RuntimeIr.h"
#include "Engine/Session/RuntimeSession.h"
#include "Engine/Support/Utf8Path.h"
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

void WriteRepeated(const std::filesystem::path& path, const char value,
                   std::uint64_t bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    const std::string block(256 * 1024, value);
    while (bytes > 0) {
        const auto count = static_cast<std::streamsize>(
            std::min<std::uint64_t>(bytes, block.size()));
        output.write(block.data(), count);
        bytes -= static_cast<std::uint64_t>(count);
    }
}

std::uint64_t PeakResidentBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    return GetProcessMemoryInfo(GetCurrentProcess(), &counters,
                                sizeof(counters))
               ? counters.PeakWorkingSetSize
               : 0;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#ifdef __APPLE__
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024u;
#endif
#endif
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
              << R"("cacheSourceDir":")" << output.generic_string() << R"(",)"
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
         suite.Expect(parsed.request.cacheSourceDir == output,
                      "optional external cache source survives wire parsing");
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

    suite.Run("ValidationOnly_ChecksNativeSemanticsWithoutPublishing", [&] {
        std::vector<px::sdk::PackageEvent> validationEvents;
        const auto valid = px::sdk::ValidatePackageRequest(
            request, [&](const auto& event) {
                validationEvents.push_back(event);
            });
        suite.Require(valid.Completed(), "valid request passes native preflight");
        suite.Expect(!validationEvents.empty() &&
                         validationEvents.back().kind ==
                             px::sdk::PackageEventKind::Validated &&
                         validationEvents.back().inputCount ==
                             request.inputs.size(),
                     "preflight emits a distinct validated event");
        suite.Expect(nlohmann::json::parse(px::sdk::SerializePackageEvent(
                         validationEvents.back()))["event"] == "validated" &&
                         nlohmann::json::parse(px::sdk::SerializePackageEvent(
                             validationEvents.back()))["deferredMediaCount"] >= 1,
                     "validated event has a machine-readable protocol name");
        suite.Expect(std::filesystem::exists(output / "previous-success.txt") &&
                         !std::filesystem::exists(output / "Content.pdx"),
                     "preflight leaves the existing output untouched");

        auto malformed = request;
        const auto source = root / "Content/UI/Title.pxui";
        const auto original = Read(source);
        Write(source, R"({"format":"InvalidUi"})");
        for (auto& input : malformed.inputs) {
            if (input.uri == "Content/UI/Title.pxui") {
                input.size = std::filesystem::file_size(source);
                input.fingerprint =
                    px::sdk::ComputePackageFingerprint(source);
            }
        }
        const auto invalid = px::sdk::ValidatePackageRequest(malformed);
        suite.Expect(!invalid.Completed() && !invalid.diagnostics.empty(),
                     "preflight rejects a malformed authored UI even when its hash matches");
        suite.Expect(std::filesystem::exists(output / "previous-success.txt") &&
                         !std::filesystem::exists(output / "Content.pdx"),
                     "failed preflight also leaves output untouched");
        Write(source, original);

        const auto media = root / "Assets/rin.png";
        const auto originalMedia = Read(media);
        Write(media, std::string(originalMedia.size(), 'x'));
        const auto structural = px::sdk::ValidatePackageRequest(request);
        const auto full = px::sdk::ValidatePackageRequest(
            request, {}, px::sdk::PackageValidationDepth::AllInputs);
        suite.Expect(structural.Completed(),
                     "fast preflight deliberately defers media payload hashes");
        suite.Expect(!full.Completed() && HasDiagnostic(full, "PXPKG1216"),
                     "full preflight rejects changed media bytes of the same size");
        Write(media, originalMedia);
        std::vector<px::sdk::PackageEvent> fullEvents;
        const auto intactFull = px::sdk::ValidatePackageRequest(
            request, [&](const auto& event) { fullEvents.push_back(event); },
            px::sdk::PackageValidationDepth::AllInputs);
        suite.Expect(intactFull.Completed() && !fullEvents.empty() &&
                         fullEvents.back().deferredMediaCount == 0,
                     "full preflight checks all current input fingerprints");
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
        const auto hasStructureProgress = [&](const std::uint64_t current) {
            return std::any_of(events.begin(), events.end(), [&](const auto& event) {
                return event.kind == px::sdk::PackageEventKind::Progress &&
                       event.phase == "validate-structure" &&
                       event.current == current && event.total == 1;
            });
        };
        suite.Expect(hasStructureProgress(0) && hasStructureProgress(1),
                     "semantic validation reports a bounded phase after input verification");
        const auto hasPhaseProgress = [&](const std::string_view phase,
                                          const std::uint64_t current) {
            return std::any_of(events.begin(), events.end(), [&](const auto& event) {
                return event.kind == px::sdk::PackageEventKind::Progress &&
                       event.phase == phase && event.current == current &&
                       event.total == 1;
            });
        };
        suite.Expect(hasPhaseProgress("validate-locales", 0) &&
                         hasPhaseProgress("validate-locales", 1) &&
                         hasPhaseProgress("validate-characters", 0) &&
                         hasPhaseProgress("validate-characters", 1),
                     "semantic validation reports locale and character subphase progress");
        suite.Expect(std::any_of(events.begin(), events.end(), [](const auto& event) {
                         return event.kind == px::sdk::PackageEventKind::Progress &&
                                event.phase == "validate-primary-reuse" &&
                                event.current == 1 && event.total == 1;
                     }),
                     "byte-identical primary Runtime IR and source map reuse the validated locale pair");

        const auto manifest = px::sdk::detail::ParsePackageManifest(
            Read(output / "Package/manifest.json"));
        suite.Require(manifest.Valid(), "Player parses the generated package manifest");
        suite.Expect(
            manifest.manifest.engineVersion == "0.2.0" &&
                manifest.manifest.startRuntimeIr == request.startScript &&
                manifest.manifest.startRoute == "title" &&
                manifest.manifest.gameId == request.gameId &&
                !manifest.manifest.packageFingerprint.empty(),
            "package manifest retains entrypoint and stable save identity");
        const auto* sceneReference = manifest.manifest.routes.size() == 1
                                         ? &manifest.manifest.routes.front()
                                         : nullptr;
        suite.Expect(sceneReference && sceneReference->sceneId == "4ff53084-a967-4ff1-88e5-266d083987b4" && sceneReference->scene == "Content/UI/Title.pxui", "startRoute maps to the packaged UI document identity");

        px::io::Archive archive;
        const auto key = px::crypto::DeriveKey(manifest.manifest.archiveKey);
        suite.Require(archive.Open((output / "Content.pdx").string(), &key), "Player archive reader opens generated PDX5");
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

    suite.Run("PackageOverlay_VerifiesExactBaseTargetAndChangedContent", [&] {
        using Json = nlohmann::json;
        const auto documents = fixture.path / "OverlayDocuments";
        const auto hashText = [&](const std::string& name,
                                  const std::string& value) {
            const auto path = documents / name;
            Write(path, value);
            return px::sdk::ComputePackageFingerprint(path);
        };
        const std::string baseManifest = Read(output / "Package/manifest.json");
        const std::string baseIndex = Read(output / "Package/content-index.json");
        Json targetManifest = Json::parse(baseManifest);
        targetManifest["packageFingerprint"] = std::string(64, 'a');
        targetManifest["contentVersion"] = "overlay-fixture-v2";
        const std::string targetManifestText = targetManifest.dump();
        Json targetIndex = Json::parse(baseIndex);
        targetIndex["packageFingerprint"] = std::string(64, 'a');
        const std::string changedStory = "雪: updated through overlay\n[end]\n";
        const std::string addedResource = "new resource";
        const auto changedHash = hashText("changed-story.txt", changedStory);
        const auto addedHash = hashText("added-resource.txt", addedResource);
        Json entries = Json::array();
        for (auto entry : targetIndex["entries"]) {
            const auto uri = entry["uri"].get<std::string>();
            if (uri == "Story/story.pxindex") continue;
            if (uri == "Story/Entry.pxstory") {
                entry["fingerprint"] = changedHash;
                entry["size"] = changedStory.size();
            }
            entries.push_back(std::move(entry));
        }
        const std::string targetArchive =
            targetIndex["entries"][0]["archive"].get<std::string>();
        entries.push_back({{"uri", "Content/Overlay/extra.txt"},
                           {"fingerprint", addedHash},
                           {"size", addedResource.size()},
                           {"archive", targetArchive}});
        std::sort(entries.begin(), entries.end(), [](const Json& a,
                                                     const Json& b) {
            return a["uri"].get<std::string>() <
                   b["uri"].get<std::string>();
        });
        targetIndex["entries"] = entries;
        const std::string targetIndexText = targetIndex.dump();
        Json metadata{{"format", "PrismatiXPackageOverlay"},
                      {"schemaRevision", 1},
                      {"gameId", targetManifest["gameId"]},
                      {"basePackageFingerprint",
                       Json::parse(baseManifest)["packageFingerprint"]},
                      {"targetPackageFingerprint", std::string(64, 'a')},
                      {"baseManifestSha256", hashText("base-manifest.txt", baseManifest)},
                      {"baseContentIndexSha256", hashText("base-index.txt", baseIndex)},
                      {"targetManifestSha256", hashText("target-manifest.txt", targetManifestText)},
                      {"targetContentIndexSha256", hashText("target-index.txt", targetIndexText)}};
        const auto writeOverlay = [&](const std::filesystem::path& path,
                                      const Json& descriptor,
                                      const std::string& story,
                                      const bool extraEntry) {
            px::io::ArchiveWriter writer;
            const auto add = [&](const std::string& uri,
                                 const std::string& data) {
                writer.Add(uri, px::io::Bytes(data.begin(), data.end()));
            };
            add("__prismatix_overlay__/manifest.json", descriptor.dump());
            add("__prismatix_overlay__/target-manifest.json", targetManifestText);
            add("__prismatix_overlay__/target-content-index.json", targetIndexText);
            add("Story/Entry.pxstory", story);
            add("Content/Overlay/extra.txt", addedResource);
            if (extraEntry) add("unexpected.txt", "unexpected");
            return writer.Write(path.string());
        };
        const auto overlayPath = fixture.path / "overlay.pdx";
        suite.Require(writeOverlay(overlayPath, metadata, changedStory, false),
                      "valid overlay fixture is writable");
        const auto verified = px::io::VerifyPackageOverlay(output, overlayPath);
        suite.Require(verified.valid && verified.changedResources == 2 &&
                          verified.tombstones ==
                              std::vector<std::string>{"Story/story.pxindex"},
                      "overlay verifier derives additions, modifications and removals");
        const auto base = px::sdk::detail::ParsePackageManifest(baseManifest);
        suite.Require(base.Valid(), "base manifest remains valid");
        const auto key = px::crypto::DeriveKey(base.manifest.archiveKey);
        px::io::VFS vfs;
        suite.Require(vfs.MountArchive((output / "Content.pdx").string(), &key) &&
                          vfs.MountArchive(overlayPath.string(), nullptr,
                              px::io::ArchiveMountPriority::Overlay,
                              verified.tombstones),
                      "verified overlay mounts over the encrypted base archive");
        suite.Expect(vfs.ReadText("Story/Entry.pxstory") == changedStory &&
                         vfs.ReadText("Content/Overlay/extra.txt") == addedResource &&
                         !vfs.Exists("Story/story.pxindex"),
                     "verified overlay changes runtime lookup and hides deleted resources");

        Json wrongBase = metadata;
        wrongBase["basePackageFingerprint"] = std::string(64, 'b');
        const auto wrongBasePath = fixture.path / "wrong-base-overlay.pdx";
        suite.Require(writeOverlay(wrongBasePath, wrongBase, changedStory, false),
                      "wrong-base overlay fixture is writable");
        suite.Expect(!px::io::VerifyPackageOverlay(output, wrongBasePath).valid,
                     "overlay verifier refuses a different base package");
        const auto badPayloadPath = fixture.path / "bad-payload-overlay.pdx";
        suite.Require(writeOverlay(badPayloadPath, metadata, "tampered", false),
                      "tampered overlay fixture is writable");
        suite.Expect(!px::io::VerifyPackageOverlay(output, badPayloadPath).valid,
                     "overlay verifier refuses payload differing from target index");
        const auto extraPath = fixture.path / "extra-entry-overlay.pdx";
        suite.Require(writeOverlay(extraPath, metadata, changedStory, true),
                      "extra-entry overlay fixture is writable");
        suite.Expect(!px::io::VerifyPackageOverlay(output, extraPath).valid,
                     "overlay verifier refuses unindexed payload");

        Json missingRouteIndex = targetIndex;
        Json missingRouteEntries = Json::array();
        for (const auto& entry : missingRouteIndex["entries"])
            if (entry["uri"] != "Content/UI/Title.pxui")
                missingRouteEntries.push_back(entry);
        missingRouteIndex["entries"] = std::move(missingRouteEntries);
        const std::string missingRouteText = missingRouteIndex.dump();
        Json missingRouteMetadata = metadata;
        missingRouteMetadata["targetContentIndexSha256"] =
            hashText("missing-route-index.txt", missingRouteText);
        px::io::ArchiveWriter missingRouteWriter;
        const auto addMissingRoute = [&](const std::string& uri,
                                         const std::string& bytes) {
            missingRouteWriter.Add(uri,
                                   px::io::Bytes(bytes.begin(), bytes.end()));
        };
        addMissingRoute("__prismatix_overlay__/manifest.json",
                        missingRouteMetadata.dump());
        addMissingRoute("__prismatix_overlay__/target-manifest.json",
                        targetManifestText);
        addMissingRoute("__prismatix_overlay__/target-content-index.json",
                        missingRouteText);
        addMissingRoute("Story/Entry.pxstory", changedStory);
        addMissingRoute("Content/Overlay/extra.txt", addedResource);
        const auto missingRoutePath = fixture.path / "missing-route-overlay.pdx";
        suite.Require(missingRouteWriter.Write(missingRoutePath.string()),
                      "missing-route overlay fixture is writable");
        suite.Expect(!px::io::VerifyPackageOverlay(output, missingRoutePath).valid,
                     "overlay verifier refuses a target missing its route scene");
    });

    suite.Run("PackageOverlay_CreatesVerifiedSingleFileFromDistributions", [&] {
        const auto sourceImage = root / "Assets/rin.png";
        const auto sourceProject = root / "project.pxproject";
        const std::string originalImage = Read(sourceImage);
        const std::string originalProject = Read(sourceProject);
        Write(sourceImage, "updated-image");
        auto changedProject = nlohmann::json::parse(originalProject);
        changedProject["contentVersion"] = "fixture-v2";
        Write(sourceProject, changedProject.dump());
        Write(root / "Content/Overlay/extra.txt", "new content");
        const auto targetOutput = fixture.path / "Build/OverlayTarget";
        auto targetRequest = FixtureRequest(root, targetOutput);
        targetRequest.requestId = "overlay-target-contract";
        targetRequest.contentVersion = "fixture-v2";
        targetRequest.inputs.push_back(
            Input(root, "Content/Overlay/extra.txt"));
        const auto targetBuild = px::sdk::RunPackager(targetRequest);
        const auto deletionTarget = fixture.path / "Build/OverlayDeletionTarget";
        auto deletionRequest = FixtureRequest(root, deletionTarget);
        deletionRequest.requestId = "overlay-deletion-target-contract";
        deletionRequest.contentVersion = "fixture-v2";
        const auto deletionBuild = px::sdk::RunPackager(deletionRequest);
        Write(sourceImage, originalImage);
        Write(sourceProject, originalProject);
        suite.Require(targetBuild.Completed() && deletionBuild.Completed(),
                      "Target distributions with changed, added, and removed resources build");

        const auto overlayPath = fixture.path / "created-overlay.pdx";
        std::vector<std::string> phases;
        const auto created = px::io::CreatePackageOverlay(
            output, targetOutput, overlayPath,
            [&](const std::string_view phase, std::uint64_t,
                std::uint64_t, std::uint64_t) {
                phases.emplace_back(phase);
            });
        suite.Require(created.created && created.changedResources >= 3 &&
                          created.removedResources == 0 &&
                          std::filesystem::is_regular_file(overlayPath),
                      "Native creator publishes a single changed-resource PDX");
        suite.Expect(std::find(phases.begin(), phases.end(), "verify-base") !=
                         phases.end() &&
                         std::find(phases.begin(), phases.end(), "verify-target") !=
                             phases.end() &&
                         std::find(phases.begin(), phases.end(), "plan-changed") !=
                             phases.end() &&
                         std::find(phases.begin(), phases.end(), "write-overlay") !=
                             phases.end(),
                     "Overlay creation reports verification, streaming plan and write phases");
        const auto verified = px::io::VerifyPackageOverlay(output, overlayPath);
        suite.Require(verified.valid &&
                          verified.changedResources == created.changedResources,
                      "Created PDX passes the independent overlay verifier");
        const auto baseManifest = px::sdk::detail::ParsePackageManifest(
            Read(output / "Package/manifest.json"));
        suite.Require(baseManifest.Valid(), "Base package manifest is readable");
        const auto baseKey =
            px::crypto::DeriveKey(baseManifest.manifest.archiveKey);
        px::io::VFS layered;
        suite.Require(layered.MountArchive((output / "Content.pdx").string(),
                                           &baseKey) &&
                          layered.MountArchive(
                              overlayPath.string(), nullptr,
                              px::io::ArchiveMountPriority::Overlay,
                              verified.tombstones),
                      "Created overlay mounts over the encrypted base");
        suite.Expect(layered.ReadText("Assets/rin.png") == "updated-image" &&
                         layered.ReadText("Content/Overlay/extra.txt") ==
                             "new content",
                     "Created overlay exposes changed and added target resources");

        const auto removalPath = fixture.path / "removal-only-overlay.pdx";
        const auto removal = px::io::CreatePackageOverlay(
            targetOutput, deletionTarget, removalPath);
        suite.Require(removal.created && removal.changedResources == 0 &&
                          removal.removedResources == 1,
                      "Removal-only update creates a metadata-only PDX");
        const auto checkedRemoval =
            px::io::VerifyPackageOverlay(targetOutput, removalPath);
        suite.Require(checkedRemoval.valid &&
                          checkedRemoval.tombstones ==
                              std::vector<std::string>{
                                  "Content/Overlay/extra.txt"},
                      "Removal-only overlay derives the deleted resource");
        const auto newerManifest = px::sdk::detail::ParsePackageManifest(
            Read(targetOutput / "Package/manifest.json"));
        suite.Require(newerManifest.Valid(),
                      "Removal-only base manifest is readable");
        const auto newerKey =
            px::crypto::DeriveKey(newerManifest.manifest.archiveKey);
        px::io::VFS removed;
        suite.Require(removed.MountArchive(
                          (targetOutput / "Content.pdx").string(), &newerKey) &&
                          removed.MountArchive(
                              removalPath.string(), nullptr,
                              px::io::ArchiveMountPriority::Overlay,
                              checkedRemoval.tombstones),
                      "Removal-only overlay mounts over its exact base");
        suite.Expect(!removed.Exists("Content/Overlay/extra.txt") &&
                         removed.ReadText("Assets/rin.png") == "updated-image",
                     "Removal-only overlay hides the deleted asset without touching others");

        const auto existing = fixture.path / "existing-overlay.pdx";
        Write(existing, "keep");
        const auto refused = px::io::CreatePackageOverlay(
            output, targetOutput, existing);
        suite.Expect(!refused.created && Read(existing) == "keep",
                     "Overlay creation never replaces an existing output");
        const auto lateOutput = fixture.path / "late-overlay.pdx";
        bool createdDuringBuild = false;
        const auto lateRefused = px::io::CreatePackageOverlay(
            output, targetOutput, lateOutput,
            [&](const std::string_view phase, const std::uint64_t current,
                const std::uint64_t total, const std::uint64_t) {
                if (!createdDuringBuild && phase == "write-overlay" &&
                    total > 0 && current == total) {
                    Write(lateOutput, "competing-output");
                    createdDuringBuild = true;
                }
            });
        suite.Expect(createdDuringBuild && !lateRefused.created &&
                         Read(lateOutput) == "competing-output",
                     "Overlay publication does not replace a late competing output");
        const auto targetPlayer =
            targetOutput / targetRequest.playerExecutable.filename();
        const std::string originalPlayer = Read(targetPlayer);
        Write(targetPlayer, "changed-runtime");
        const auto changedRuntimePath = fixture.path / "runtime-change.pdx";
        const auto runtimeRefused = px::io::CreatePackageOverlay(
            output, targetOutput, changedRuntimePath);
        Write(targetPlayer, originalPlayer);
        suite.Expect(!runtimeRefused.created &&
                         !std::filesystem::exists(changedRuntimePath),
                     "Content-only overlay refuses a changed Player binary");

        const auto targetManifestPath = targetOutput / "Package/manifest.json";
        const std::string targetManifest = Read(targetManifestPath);
        bool changedDuringVerification = false;
        const auto changedMetadataPath = fixture.path / "changed-metadata.pdx";
        const auto metadataRefused = px::io::CreatePackageOverlay(
            output, targetOutput, changedMetadataPath,
            [&](const std::string_view phase, const std::uint64_t current,
                const std::uint64_t total, const std::uint64_t) {
                if (!changedDuringVerification && phase == "verify-target" &&
                    current == total) {
                    Write(targetManifestPath, targetManifest + " ");
                    changedDuringVerification = true;
                }
            });
        Write(targetManifestPath, targetManifest);
        suite.Expect(changedDuringVerification && !metadataRefused.created &&
                         !std::filesystem::exists(changedMetadataPath),
                     "Overlay creation refuses metadata changed after content verification");
    });

    suite.Run("PackagePublisherKey_IsPinnedInManifestAndPackageIdentity", [&] {
        constexpr std::string_view publicKey =
            "046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
            "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5";
        const auto pinnedOutput = fixture.path / "Build/PublisherPinned";
        auto pinned = FixtureRequest(root, pinnedOutput);
        pinned.requestId = "publisher-pin-contract";
        pinned.publisherPublicKey = std::string(publicKey);
        const auto built = px::sdk::RunPackager(pinned);
        suite.Require(built.Completed(),
                      "Packager accepts a pinned P-256 publisher public key");
        const auto baseManifest = px::sdk::detail::ParsePackageManifest(
            Read(output / "Package/manifest.json"));
        const auto pinnedManifest = px::sdk::detail::ParsePackageManifest(
            Read(pinnedOutput / "Package/manifest.json"));
        suite.Require(baseManifest.Valid() && pinnedManifest.Valid(),
                      "both package manifests parse");
        suite.Expect(pinnedManifest.manifest.publisherPublicKey == publicKey &&
                         pinnedManifest.manifest.packageFingerprint !=
                             baseManifest.manifest.packageFingerprint,
                     "publisher key is pinned in the manifest and package identity");
        const auto rotatedOverlay = fixture.path / "rotated-key-overlay.pdx";
        const auto rotated = px::io::CreatePackageOverlay(
            output, pinnedOutput, rotatedOverlay);
        suite.Expect(!rotated.created &&
                         !std::filesystem::exists(rotatedOverlay),
                     "content overlay cannot introduce or rotate the publisher key");
        pinned.publisherPublicKey = "04not-a-public-key";
        pinned.outputDir = fixture.path / "Build/InvalidPublisherKey";
        const auto invalid = px::sdk::RunPackager(pinned);
        suite.Expect(!invalid.Completed() &&
                         HasDiagnostic(invalid, "PXPKG1243") &&
                         !std::filesystem::exists(pinned.outputDir),
                     "invalid publisher key fails before a distribution is written");
        pinned.publisherPublicKey = "04" + std::string(128, '0');
        pinned.outputDir = fixture.path / "Build/OffCurvePublisherKey";
        const auto offCurve = px::sdk::RunPackager(pinned);
        suite.Expect(!offCurve.Completed() &&
                         HasDiagnostic(offCurve, "PXPKG1243") &&
                         !std::filesystem::exists(pinned.outputDir),
                     "a correctly shaped off-curve point is rejected");
    });

    suite.Run("SignedPackageOverlay_RequiresMatchingPublisherKeyAndExactBase", [&] {
        const auto keys = px::io::GenerateOverlaySigningKeyPair();
        suite.Require(keys.has_value(), "P-256 overlay signing keys can be generated");
        const auto keyFile = fixture.path / "publisher-private-key.txt";
        Write(keyFile, keys->privateKeyHex + "\n");
        const auto signedBase = fixture.path / "Build/SignedOverlayBase";
        auto baseRequest = FixtureRequest(root, signedBase);
        baseRequest.publisherPublicKey = keys->publicKeyHex;
        suite.Require(px::sdk::RunPackager(baseRequest).Completed(),
                      "signed overlay base package is built");
        Write(root / "Content/Overlay/signed.txt", "signed update content");
        const auto signedTarget = fixture.path / "Build/SignedOverlayTarget";
        auto targetRequest = FixtureRequest(root, signedTarget);
        targetRequest.publisherPublicKey = keys->publicKeyHex;
        targetRequest.inputs.push_back(Input(root, "Content/Overlay/signed.txt"));
        suite.Require(px::sdk::RunPackager(targetRequest).Completed(),
                      "signed overlay target package is built");
        const auto patch = fixture.path / "signed-overlay.pdx";
        const auto created = px::io::CreatePackageOverlay(
            signedBase, signedTarget, patch, {}, keyFile);
        suite.Require(created.created, "signed overlay is created: " + created.error);
        const auto verified = px::io::VerifySignedPackageOverlay(signedBase, patch);
        suite.Expect(verified.valid && verified.changedResources == 1,
                     "signed overlay validates its publisher and changed content");
        suite.Expect(!px::io::VerifySignedPackageOverlay(output, patch).valid,
                     "signed overlay rejects a different exact base");
        px::io::Archive signedArchive;
        suite.Require(signedArchive.Open(px::PathToUtf8(patch)),
                      "signed PDX can be opened for tamper test");
        const auto signature = signedArchive.Read(
            px::io::kPackageOverlaySignatureUri);
        suite.Require(signature && signature->size() == 64,
                      "signed PDX contains a 64-byte signature");
        auto tamperedBytes = Read(patch);
        const std::string signatureBytes(signature->begin(), signature->end());
        const auto signatureOffset = tamperedBytes.find(signatureBytes);
        suite.Require(signatureOffset != std::string::npos,
                      "signature bytes occur in the uncompressed PDX");
        tamperedBytes[signatureOffset] ^= 1;
        const auto tamperedPatch = fixture.path / "tampered-signed-overlay.pdx";
        Write(tamperedPatch, tamperedBytes);
        suite.Expect(!px::io::VerifySignedPackageOverlay(
                          signedBase, tamperedPatch).valid,
                     "signed verifier rejects a modified signature");
        const auto wrongKeys = px::io::GenerateOverlaySigningKeyPair();
        suite.Require(wrongKeys.has_value(), "second P-256 key pair is generated");
        const auto wrongKeyFile = fixture.path / "wrong-private-key.txt";
        Write(wrongKeyFile, wrongKeys->privateKeyHex);
        const auto wrongPatch = fixture.path / "wrong-signed-overlay.pdx";
        const auto rejected = px::io::CreatePackageOverlay(
            signedBase, signedTarget, wrongPatch, {}, wrongKeyFile);
        suite.Expect(!rejected.created && !std::filesystem::exists(wrongPatch),
                     "signer cannot publish with a key other than the pinned key");
        const auto unsignedPatch = fixture.path / "unsigned-pinned-overlay.pdx";
        suite.Require(px::io::CreatePackageOverlay(
                          signedBase, signedTarget, unsignedPatch).created,
                      "explicit unsigned overlay can be created for inspection");
        suite.Expect(!px::io::VerifySignedPackageOverlay(
                          signedBase, unsignedPatch).valid,
                     "Player signed verifier rejects an unsigned overlay");

        Write(root / "Content/Overlay/signed-second.txt", "second update");
        const auto secondTarget = fixture.path / "Build/SignedOverlaySecondTarget";
        auto secondRequest = targetRequest;
        secondRequest.outputDir = secondTarget;
        secondRequest.inputs.push_back(Input(root, "Content/Overlay/signed-second.txt"));
        suite.Require(px::sdk::RunPackager(secondRequest).Completed(),
                      "second target distribution is built");
        const auto secondPatch = fixture.path / "second-signed-overlay.pdx";
        suite.Require(px::io::CreatePackageOverlay(
                          signedTarget, secondTarget, secondPatch, {}, keyFile)
                          .created,
                      "second exact-base update is signed");
        const auto fromDocuments =
            px::io::VerifySignedPackageOverlayFromDocuments(
                verified.targetManifestJson, verified.targetContentIndexJson,
                secondPatch);
        suite.Expect(fromDocuments.valid &&
                         fromDocuments.changedResources == 1,
                     "second update verifies against the first signed target documents");
        const auto updates = fixture.path / "SignedUpdates";
        std::filesystem::create_directories(updates);
        std::filesystem::copy_file(patch, updates / "z-patch.pdx");
        std::filesystem::copy_file(secondPatch, updates / "a-patch2.pdx");
        const auto chain = px::io::ResolveSignedPackageOverlayChain(
            signedBase, updates);
        const auto finalManifest = px::sdk::detail::ParsePackageManifest(
            Read(secondTarget / "Package/manifest.json"));
        const auto baseManifest = px::sdk::detail::ParsePackageManifest(
            Read(signedBase / "Package/manifest.json"));
        const auto firstManifest = px::sdk::detail::ParsePackageManifest(
            Read(signedTarget / "Package/manifest.json"));
        suite.Expect(chain.valid && chain.mounts.size() == 2 &&
                         chain.mounts[0].path.filename() == "z-patch.pdx" &&
                         chain.mounts[1].path.filename() == "a-patch2.pdx" &&
                         finalManifest.Valid() &&
                         px::sdk::detail::ParsePackageManifest(
                             chain.targetManifestJson)
                                 .manifest.packageFingerprint ==
                             finalManifest.manifest.packageFingerprint,
                     "update chain follows signed exact-base dependencies, not filenames");
        suite.Expect(baseManifest.Valid() && firstManifest.Valid() &&
                         chain.compatibleSaveFingerprints ==
                             std::vector<std::string>{
                                 baseManifest.manifest.packageFingerprint,
                                 firstManifest.manifest.packageFingerprint},
                     "signed same-version ancestors remain eligible for save loading");
        std::filesystem::copy_file(unsignedPatch, updates / "unsigned.pdx");
        const auto withUnsigned = px::io::ResolveSignedPackageOverlayChain(
            signedBase, updates);
        suite.Expect(withUnsigned.valid && withUnsigned.mounts.size() == 2 &&
                         withUnsigned.ignoredFiles.size() == 1,
                     "unsigned file is ignored without hiding a valid chain");
        px::io::Archive mainArchive;
        suite.Require(mainArchive.Open(px::PathToUtf8(patch)),
                      "signed PDX opens for an exact-base signature check");
        const auto signedSignature = mainArchive.Read(
            px::io::kPackageOverlaySignatureUri);
        suite.Require(signedSignature && signedSignature->size() == 64,
                      "signed PDX has a publisher signature");
        auto damagedMainBytes = Read(patch);
        const auto signedSignatureOffset = damagedMainBytes.find(std::string(
            signedSignature->begin(), signedSignature->end()));
        suite.Require(signedSignatureOffset != std::string::npos,
                      "main signature can be located in the PDX fixture");
        damagedMainBytes[signedSignatureOffset] ^= 1;
        Write(updates / "bad-signature.pdx", damagedMainBytes);
        const auto withDamagedMain = px::io::ResolveSignedPackageOverlayChain(
            signedBase, updates);
        suite.Expect(withDamagedMain.valid &&
                         withDamagedMain.mounts.size() == 2 &&
                         withDamagedMain.ignoredFiles.size() == 2,
                     "matching base hint cannot bypass the main update signature");
        std::filesystem::remove(updates / "bad-signature.pdx");

        Write(root / "Content/Overlay/adult.txt", "optional adult content");
        const auto adultTarget = fixture.path / "Build/SignedOverlayAdultTarget";
        auto adultRequest = secondRequest;
        adultRequest.outputDir = adultTarget;
        adultRequest.inputs.push_back(Input(root, "Content/Overlay/adult.txt"));
        suite.Require(px::sdk::RunPackager(adultRequest).Completed(),
                      "optional target distribution is built");
        const auto adultPatch = fixture.path / "adult.pdx";
        suite.Require(px::io::CreatePackageOverlay(
                          secondTarget, adultTarget, adultPatch, {}, keyFile,
                          "adult").created,
                      "adult pack is signed with an explicit optional ID");
        std::filesystem::copy_file(adultPatch, updates / "adult.pdx");
        const auto disabledAdult = px::io::ResolveSignedPackageOverlayChain(
            signedBase, updates);
        suite.Expect(disabledAdult.valid && disabledAdult.mounts.size() == 2 &&
                         disabledAdult.optionalPackId.empty() &&
                         disabledAdult.ignoredFiles.size() == 1,
                     "installed optional pack stays off by default");
        const auto enabledAdult = px::io::ResolveSignedPackageOverlayChain(
            signedBase, updates, "adult");
        const auto adultManifest = px::sdk::detail::ParsePackageManifest(
            Read(adultTarget / "Package/manifest.json"));
        suite.Expect(enabledAdult.valid && enabledAdult.mounts.size() == 3 &&
                         enabledAdult.mounts.back().path.filename() == "adult.pdx" &&
                         enabledAdult.optionalPackId == "adult" &&
                         enabledAdult.mainPackageFingerprint ==
                             finalManifest.manifest.packageFingerprint &&
                         adultManifest.Valid() &&
                         px::sdk::detail::ParsePackageManifest(
                             enabledAdult.targetManifestJson)
                                 .manifest.packageFingerprint ==
                             adultManifest.manifest.packageFingerprint,
                     "enabled optional pack mounts after updates without changing save identity");
        const auto otherOptional = px::io::ResolveSignedPackageOverlayChain(
            signedBase, updates, "other");
        suite.Expect(otherOptional.valid && otherOptional.mounts.size() == 2,
                     "an unrelated optional selection leaves the main chain intact");
        px::io::Archive adultArchive;
        suite.Require(adultArchive.Open(px::PathToUtf8(adultPatch)),
                      "optional PDX opens for a damaged-signature check");
        const auto adultSignature = adultArchive.Read(
            px::io::kPackageOverlaySignatureUri);
        suite.Require(adultSignature && adultSignature->size() == 64,
                      "optional PDX has a publisher signature");
        auto adultBytes = Read(adultPatch);
        const auto adultSignatureOffset = adultBytes.find(std::string(
            adultSignature->begin(), adultSignature->end()));
        suite.Require(adultSignatureOffset != std::string::npos,
                      "optional signature can be located in the PDX fixture");
        adultBytes[adultSignatureOffset] ^= 1;
        Write(updates / "adult.pdx", adultBytes);
        const auto damagedDisabled = px::io::ResolveSignedPackageOverlayChain(
            signedBase, updates);
        suite.Expect(damagedDisabled.valid &&
                         damagedDisabled.mounts.size() == 2 &&
                         damagedDisabled.ignoredFiles.size() == 1,
                     "disabled optional pack does not block main updates");
        const auto damagedEnabled = px::io::ResolveSignedPackageOverlayChain(
            signedBase, updates, "adult");
        suite.Expect(damagedEnabled.valid &&
                         damagedEnabled.mounts.size() == 2 &&
                         damagedEnabled.ignoredFiles.size() == 2,
                     "enabled optional pack with a bad signature is ignored");
        suite.Expect(!px::io::CreatePackageOverlay(
                          secondTarget, adultTarget,
                          fixture.path / "unsigned-adult.pdx", {}, {}, "adult")
                          .created,
                     "optional pack cannot be published without a signature");

        Write(root / "Content/Overlay/branch.txt", "ambiguous branch");
        const auto branchTarget = fixture.path / "Build/SignedOverlayBranch";
        auto branchRequest = FixtureRequest(root, branchTarget);
        branchRequest.publisherPublicKey = keys->publicKeyHex;
        branchRequest.inputs.push_back(Input(root, "Content/Overlay/branch.txt"));
        suite.Require(px::sdk::RunPackager(branchRequest).Completed(),
                      "alternative signed target distribution is built");
        const auto branchPatch = fixture.path / "branch-signed-overlay.pdx";
        suite.Require(px::io::CreatePackageOverlay(
                          signedBase, branchTarget, branchPatch, {}, keyFile)
                          .created,
                      "alternative signed update is built");
        std::filesystem::copy_file(branchPatch, updates / "branch.pdx");
        suite.Expect(!px::io::ResolveSignedPackageOverlayChain(
                          signedBase, updates).valid,
                     "two valid updates from the same base are rejected as ambiguous");
    });

    suite.Run("StableArchiveShardsMountAndReuseTheirOwnCaches", [&] {
        WriteRepeated(root / "Assets/shard-compress.bin", 's', 256 * 1024);
        const auto shardedOutput = fixture.path / "Build/Sharded";
        auto sharded = FixtureRequest(root, shardedOutput);
        sharded.requestId = "sharded-archive-contract";
        sharded.encryption = false;
        sharded.compression = px::sdk::PackageCompression::Fast;
        sharded.archiveShards = 4;
        sharded.inputs.push_back(Input(root, "Assets/shard-compress.bin"));
        suite.Require(px::sdk::RunPackager(sharded).Completed(),
                      "Packager creates a sharded distribution");
        const auto manifest = px::sdk::detail::ParsePackageManifest(
            Read(shardedOutput / "Package/manifest.json"));
        suite.Require(manifest.Valid() && manifest.manifest.archives.size() > 1,
                      "manifest lists multiple generated PDX archives");
        const auto verified = px::io::VerifyPackageContents(shardedOutput);
        suite.Expect(verified.valid && verified.archives ==
                         manifest.manifest.archives.size() &&
                         verified.resources == sharded.inputs.size(),
                     "native verifier reads and hashes every indexed PDX resource",
                     verified.error);
        const auto contentIndexPath = shardedOutput / "Package/content-index.json";
        const auto intactIndex = Read(contentIndexPath);
        auto alteredIndex = nlohmann::json::parse(intactIndex);
        alteredIndex["entries"][0]["fingerprint"] = std::string(64, '0');
        Write(contentIndexPath, alteredIndex.dump());
        suite.Expect(!px::io::VerifyPackageContents(shardedOutput).valid,
                     "native verifier rejects an index that lies about archive contents");
        Write(contentIndexPath, intactIndex);
        const auto corruptedArchivePath =
            shardedOutput / manifest.manifest.archives.front().file;
        const auto intactArchive = Read(corruptedArchivePath);
        auto corruptedArchive = intactArchive;
        corruptedArchive.back() ^= 1;
        Write(corruptedArchivePath, corruptedArchive);
        suite.Expect(!px::io::VerifyPackageContents(shardedOutput).valid,
                     "native verifier rejects a corrupted PDX payload");
        Write(corruptedArchivePath, intactArchive);
        const auto contentIndex = nlohmann::json::parse(
            Read(shardedOutput / "Package/content-index.json"));
        suite.Require(contentIndex.at("format") == "PrismatiXContentIndex" &&
                          contentIndex.at("schemaRevision") == 1 &&
                          contentIndex.at("gameId") == sharded.gameId &&
                          contentIndex.at("packageFingerprint") ==
                              manifest.manifest.packageFingerprint &&
                          contentIndex.at("entries").size() ==
                              sharded.inputs.size(),
                      "packaged content index enumerates the exact source set");
        px::io::VFS vfs;
        std::vector<std::pair<std::string, std::string>> fingerprints;
        for (const auto& archive : manifest.manifest.archives) {
            const auto path = shardedOutput / archive.file;
            suite.Require(std::filesystem::is_regular_file(path) &&
                              vfs.MountArchive(path.string()),
                          "each manifest shard mounts in the production VFS");
            fingerprints.emplace_back(
                archive.file, px::sdk::ComputePackageFingerprint(path));
            const auto suffix = archive.file.substr(8, 2);
            suite.Expect(std::filesystem::is_regular_file(
                             shardedOutput / "Package" /
                             ("entry-cache-" + suffix + ".json")),
                         "each compressed shard has a corresponding cache inventory");
        }
        for (const auto& input : sharded.inputs) {
            const auto bytes = vfs.Read(input.uri);
            suite.Expect(bytes &&
                             std::string(bytes->begin(), bytes->end()) ==
                                 Read(root / input.uri),
                         "all packaged inputs resolve across the shard set",
                         input.uri);
            const auto indexed = std::ranges::find_if(
                contentIndex.at("entries"), [&](const auto& item) {
                    return item.at("uri") == input.uri;
                });
            suite.Expect(indexed != contentIndex.at("entries").end() &&
                             indexed->at("fingerprint") == input.fingerprint &&
                             indexed->at("size") == input.size &&
                             std::ranges::any_of(
                                 manifest.manifest.archives,
                                 [&](const auto& archive) {
                                     return archive.file ==
                                            indexed->at("archive").get<std::string>();
                                 }),
                         "content index binds source fingerprint to a mounted shard",
                         input.uri);
        }
        vfs.Clear();
        std::vector<px::sdk::PackageEvent> events;
        suite.Require(px::sdk::RunPackager(sharded, [&](const auto& event) {
                          events.push_back(event);
                      }).Completed(),
                      "repacking the same shards succeeds");
        const auto reuse = std::ranges::find_if(events, [](const auto& event) {
            return event.phase == "archive-cache";
        });
        suite.Expect(reuse != events.end() && reuse->current > 0,
                     "sharded repack reuses verified compressed entry bytes");
        const auto wholeReuse = std::ranges::find_if(
            events, [](const auto& event) {
                return event.phase == "archive-shard-reuse";
            });
        suite.Expect(wholeReuse != events.end() &&
                         wholeReuse->current == fingerprints.size(),
                     "unchanged shards reuse whole archive files");
        for (const auto& [file, fingerprint] : fingerprints)
            suite.Expect(px::sdk::ComputePackageFingerprint(shardedOutput / file) ==
                             fingerprint,
                         "sharded archive bytes remain deterministic", file);
        auto staged = sharded;
        staged.outputDir = fixture.path / "Build/ShardedStaged";
        staged.cacheSourceDir = shardedOutput;
        events.clear();
        suite.Require(px::sdk::RunPackager(staged, [&](const auto& event) {
                          events.push_back(event);
                      }).Completed(),
                      "CLI-style staging can reuse a sharded cache source");
        const auto stagedReuse = std::ranges::find_if(
            events, [](const auto& event) { return event.phase == "archive-cache"; });
        suite.Expect(stagedReuse != events.end() && stagedReuse->current > 0,
                     "external sharded cache source reuses verified entries");
        const auto stagedWholeReuse = std::ranges::find_if(
            events, [](const auto& event) {
                return event.phase == "archive-shard-reuse";
            });
        suite.Expect(stagedWholeReuse != events.end() &&
                         stagedWholeReuse->current == 0 &&
                         !std::filesystem::equivalent(
                             shardedOutput / fingerprints.front().first,
                             staged.outputDir / fingerprints.front().first),
                     "unretired external cache source is never hard-linked");
        Write(shardedOutput / "Package/archive-shards.json", "{broken");
        events.clear();
        suite.Require(px::sdk::RunPackager(sharded, [&](const auto& event) {
                          events.push_back(event);
                      }).Completed(),
                      "malformed shard reuse metadata falls back to fresh output");
        const auto malformedReuse = std::ranges::find_if(
            events, [](const auto& event) {
                return event.phase == "archive-shard-reuse";
            });
        suite.Expect(malformedReuse != events.end() &&
                         malformedReuse->current == 0,
                     "malformed sidecar cannot authorize whole-shard reuse");
        Write(root / "Assets/shard-added.bin", "one later resource");
        sharded.inputs.push_back(Input(root, "Assets/shard-added.bin"));
        events.clear();
        suite.Require(px::sdk::RunPackager(sharded, [&](const auto& event) {
                          events.push_back(event);
                      }).Completed(),
                      "adding a resource rebuilds a sharded distribution");
        const auto partialReuse = std::ranges::find_if(
            events, [](const auto& event) {
                return event.phase == "archive-shard-reuse";
            });
        suite.Expect(partialReuse != events.end() &&
                         partialReuse->current == fingerprints.size() - 1,
                     "adding one resource rewrites only its shard");
        std::string changedShard;
        for (const auto& [file, fingerprint] : fingerprints) {
            px::io::Archive archive;
            suite.Require(archive.Open((shardedOutput / file).string()),
                          "rebuilt shard remains readable");
            if (archive.Contains("Assets/shard-added.bin")) changedShard = file;
        }
        suite.Require(!changedShard.empty(),
                      "new resource lands in one deterministic shard");
        for (const auto& [file, fingerprint] : fingerprints)
            if (file != changedShard)
                suite.Expect(px::sdk::ComputePackageFingerprint(shardedOutput / file) ==
                                 fingerprint,
                             "unaffected shards retain identical bytes", file);
        auto encrypted = sharded;
        encrypted.requestId = "sharded-encrypted-contract";
        encrypted.outputDir = fixture.path / "Build/ShardedEncrypted";
        encrypted.encryption = true;
        suite.Require(px::sdk::RunPackager(encrypted).Completed(),
                      "encrypted sharded distribution packages");
        const auto encryptedManifest = px::sdk::detail::ParsePackageManifest(
            Read(encrypted.outputDir / "Package/manifest.json"));
        suite.Require(encryptedManifest.Valid(),
                      "encrypted sharded manifest validates");
        suite.Expect(px::io::VerifyPackageContents(encrypted.outputDir).valid,
                     "native verifier hashes decrypted encrypted-shard resources");
        const auto key = px::crypto::DeriveKey(encryptedManifest.manifest.archiveKey);
        px::io::VFS encryptedVfs;
        for (const auto& mount : encryptedManifest.manifest.archives)
            suite.Require(encryptedVfs.MountArchive(
                              (encrypted.outputDir / mount.file).string(), &key),
                          "each encrypted shard mounts with the package key");
        const auto encryptedBytes = encryptedVfs.Read("Assets/shard-added.bin");
        suite.Expect(encryptedBytes &&
                         std::string(encryptedBytes->begin(), encryptedBytes->end()) ==
                             "one later resource",
                     "encrypted shard data resolves through the production VFS");
        encryptedVfs.Clear();
        events.clear();
        suite.Require(px::sdk::RunPackager(encrypted, [&](const auto& event) {
                          events.push_back(event);
                      }).Completed(),
                      "unchanged encrypted shard set repackages");
        const auto encryptedReuse = std::ranges::find_if(
            events, [](const auto& event) {
                return event.phase == "archive-shard-reuse";
            });
        suite.Expect(encryptedReuse != events.end() &&
                         encryptedReuse->current ==
                             encryptedManifest.manifest.archives.size(),
                     "unchanged encrypted shards reuse their verified bytes");
        sharded.archiveShards = 65;
        suite.Expect(HasDiagnostic(px::sdk::RunPackager(sharded), "PXPKG1241"),
                     "invalid shard counts fail before modifying output");
    });

    suite.Run("StreamingWriter_IsDeterministicAndLeavesNoWorkFiles", [&] {
        const auto repeatOutput = fixture.path / "Build/Repeat";
        auto repeat = FixtureRequest(root, repeatOutput);
        repeat.requestId = "deterministic-repeat";
        const auto result = px::sdk::RunPackager(repeat);
        suite.Require(result.Completed(),
                      "the same input set packages a second time");
        suite.Expect(
            px::sdk::ComputePackageFingerprint(output / "Content.pdx") ==
                px::sdk::ComputePackageFingerprint(repeatOutput /
                                                   "Content.pdx"),
            "streamed PDX5 output is byte-for-byte deterministic");
        suite.Expect(!std::filesystem::exists(output / ".pdx-work") &&
                         !std::filesystem::exists(repeatOutput /
                                                  ".pdx-work"),
                     "entry transform scratch data is removed before promotion");
    });

    suite.Run("ContentUpgradePreservesAndReadsLegacyUserData", [&] {
        const auto upgradeOutput = fixture.path / "Build/PersistenceUpgrade";
        auto versionOne = FixtureRequest(root, upgradeOutput);
        versionOne.requestId = "persistence-upgrade-v1";
        const auto first = px::sdk::RunPackager(versionOne);
        suite.Require(first.Completed(), "version-one package completes");

        const auto manifestPath = upgradeOutput / "Package/manifest.json";
        auto legacyManifest = nlohmann::json::parse(Read(manifestPath));
        const std::string legacyFingerprint =
            legacyManifest.at("packageFingerprint").get<std::string>();
        const std::string legacySecret =
            legacyManifest.at("encryption").at("archiveKey").get<std::string>();
        legacyManifest.erase("persistence");
        Write(manifestPath, legacyManifest.dump());
        const auto parsedLegacy = px::sdk::detail::ParsePackageManifest(
            Read(manifestPath));
        suite.Require(parsedLegacy.Valid() &&
                          parsedLegacy.manifest.persistenceKeyId.empty(),
                      "pre-stable-key package manifest remains a readable legacy fixture");

        const auto legacyKey =
            px::progress::DeriveLegacyPersistenceKey(legacySecret);
        const auto saveDirectory = upgradeOutput / "Save";
        px::progress::GlobalProfile profile;
        profile.MarkSeen("v1#line");
        profile.SetVariable("ending", px::Variant("alice"));
        suite.Require(px::progress::SaveGlobalProfile(
                          profile, (saveDirectory / "profile.dat").string(),
                          &legacyKey),
                      "version-one profile is written with its package key");
        px::progress::GameSettings settings;
        settings.language = "ja-JP";
        settings.textSpeedMs = 21;
        suite.Require(settings.Save(
                          (saveDirectory / "config.dat").string(), &legacyKey),
                      "version-one settings are written with their package key");
        px::progress::SaveSnapshot snapshot;
        snapshot.gameId = versionOne.gameId;
        snapshot.packageFingerprint = legacyFingerprint;
        snapshot.contentVersion = versionOne.contentVersion;
        snapshot.saveVersion = versionOne.saveVersion;
        snapshot.anchor = {"99999999-9999-4999-8999-999999999999",
                           "line-1", "line-1"};
        snapshot.scriptPath = versionOne.startScript;
        snapshot.vm.scriptPath = versionOne.startScript;
        snapshot.chapter = "v1";
        px::progress::SaveSystem oldSaves;
        oldSaves.Configure(saveDirectory.string(), &legacyKey);
        suite.Require(oldSaves.Save(2, snapshot),
                      "version-one slot is written with its package key");
        const std::string profileBytes =
            Read(saveDirectory / "profile.dat");
        const std::string settingsBytes =
            Read(saveDirectory / "config.dat");
        const std::string slotBytes = Read(oldSaves.SlotPath(2));

        const auto storyPath = root / "Story/Entry.pxstory";
        const std::string originalStory = Read(storyPath);
        const auto projectPath = root / "project.pxproject";
        const std::string originalProject = Read(projectPath);
        auto upgradedProject = nlohmann::json::parse(originalProject);
        upgradedProject["contentVersion"] = "fixture-v2";
        Write(storyPath, originalStory + "\n; content upgrade\n");
        Write(projectPath, upgradedProject.dump());
        const auto standaloneOutput =
            fixture.path / "Build/PersistenceStandaloneUpgrade";
        auto standaloneUpgrade = FixtureRequest(root, standaloneOutput);
        standaloneUpgrade.requestId = "persistence-standalone-upgrade-v2";
        standaloneUpgrade.contentVersion = "fixture-v2";
        standaloneUpgrade.cacheSourceDir = upgradeOutput;
        const auto legacyArchiveFingerprint =
            px::sdk::ComputePackageFingerprint(upgradeOutput / "Content.pdx");
        suite.Require(px::sdk::RunPackager(standaloneUpgrade).Completed(),
                      "standalone content upgrade can read a separate old distribution");
        const auto standaloneManifest = px::sdk::detail::ParsePackageManifest(
            Read(standaloneOutput / "Package/manifest.json"));
        suite.Require(standaloneManifest.Valid() &&
                          standaloneManifest.manifest.legacyPersistenceReaders.size() == 1 &&
                          standaloneManifest.manifest.legacyPersistenceReaders.front().secret ==
                              legacySecret,
                      "standalone distribution inherits the old persistence reader");
        const auto standaloneKey = px::progress::DeriveGameDataPersistenceKey(
            standaloneManifest.manifest.persistenceDataId);
        std::vector<px::crypto::Key> standaloneFallbackKeys;
        for (const auto& reader :
             standaloneManifest.manifest.legacyPersistenceReaders) {
            standaloneFallbackKeys.push_back(
                px::progress::DeriveLegacyPersistenceKey(reader.secret));
        }
        px::progress::SaveSystem standaloneSaves;
        standaloneSaves.Configure(saveDirectory.string(), &standaloneKey,
                                  standaloneFallbackKeys);
        const auto standaloneSlot = standaloneSaves.Load(2);
        suite.Expect(standaloneSlot && standaloneSlot->chapter == "v1" &&
                         Read(manifestPath) == legacyManifest.dump() &&
                         px::sdk::ComputePackageFingerprint(
                             upgradeOutput / "Content.pdx") ==
                             legacyArchiveFingerprint,
                     "standalone upgrade reads an old encrypted slot without changing the old package");
        auto versionTwo = FixtureRequest(root, upgradeOutput);
        versionTwo.requestId = "persistence-upgrade-v2";
        versionTwo.contentVersion = "fixture-v2";
        const auto second = px::sdk::RunPackager(versionTwo);
        Write(storyPath, originalStory);
        Write(projectPath, originalProject);
        suite.Require(second.Completed(), "content-upgraded package completes");

        const auto currentManifest = px::sdk::detail::ParsePackageManifest(
            Read(manifestPath));
        suite.Require(currentManifest.Valid(),
                      "upgraded package publishes a valid persistence contract");
        suite.Expect(
            currentManifest.manifest.packageFingerprint != legacyFingerprint &&
                currentManifest.manifest.persistenceDataId == versionTwo.gameId &&
                currentManifest.manifest.persistenceKeyId ==
                    px::progress::kGameDataPersistenceKeyId &&
                currentManifest.manifest.legacyPersistenceReaders.size() == 1 &&
                currentManifest.manifest.legacyPersistenceReaders.front().secret ==
                    legacySecret,
            "content identity changes while stable data identity retains the v1 reader");
        suite.Require(
            Read(saveDirectory / "profile.dat") == profileBytes &&
                Read(saveDirectory / "config.dat") == settingsBytes &&
                Read(oldSaves.SlotPath(2)) == slotBytes,
            "atomic output promotion preserves profile, settings, and slot bytes");

        const auto stableKey = px::progress::DeriveGameDataPersistenceKey(
            currentManifest.manifest.persistenceDataId);
        std::vector<px::crypto::Key> fallbackKeys;
        for (const auto& reader :
             currentManifest.manifest.legacyPersistenceReaders) {
            fallbackKeys.push_back(
                px::progress::DeriveLegacyPersistenceKey(reader.secret));
        }
        px::progress::GlobalProfile upgradedProfile;
        px::progress::GameSettings upgradedSettings;
        px::progress::SaveSystem upgradedSaves;
        upgradedSaves.Configure(saveDirectory.string(), &stableKey,
                                fallbackKeys);
        const auto upgradedSlot = upgradedSaves.Load(2);
        suite.Expect(
            px::progress::LoadGlobalProfileWithFallback(
                upgradedProfile, (saveDirectory / "profile.dat").string(),
                &stableKey, fallbackKeys) ==
                    px::progress::PersistLoadStatus::Loaded &&
                upgradedProfile.HasSeen("v1#line") &&
                upgradedProfile.Variable("ending") &&
                upgradedSettings.LoadWithFallback(
                    (saveDirectory / "config.dat").string(), &stableKey,
                    fallbackKeys) == px::progress::PersistLoadStatus::Loaded &&
                upgradedSettings.language == "ja-JP" && upgradedSlot &&
                upgradedSlot->chapter == "v1",
            "upgraded package readers recover all version-one user data before migration");

        const std::string priorFingerprint =
            currentManifest.manifest.packageFingerprint;
        Write(storyPath, originalStory + "\n; same-version release\n");
        Write(projectPath, upgradedProject.dump());
        auto sameVersion = FixtureRequest(root, upgradeOutput);
        sameVersion.requestId = "persistence-same-version-v2";
        sameVersion.contentVersion = "fixture-v2";
        suite.Require(px::sdk::RunPackager(sameVersion).Completed(),
                      "same-version replacement package completes");
        const auto replacementManifest = px::sdk::detail::ParsePackageManifest(
            Read(manifestPath));
        suite.Require(replacementManifest.Valid() &&
                          replacementManifest.manifest.packageFingerprint !=
                              priorFingerprint &&
                          replacementManifest.manifest.compatibleSaveFingerprints ==
                              std::vector<std::string>{priorFingerprint},
                      "same-version replacement records the former save identity");
        suite.Require(px::sdk::RunPackager(sameVersion).Completed(),
                      "repeating an unchanged package completes");
        const auto repeatedManifest = px::sdk::detail::ParsePackageManifest(
            Read(manifestPath));
        suite.Expect(repeatedManifest.Valid() &&
                         repeatedManifest.manifest.compatibleSaveFingerprints ==
                             std::vector<std::string>{priorFingerprint},
                     "repeating an unchanged package does not grow save ancestry");
        auto stagedSameVersion = sameVersion;
        stagedSameVersion.requestId = "persistence-cli-staging-v2";
        stagedSameVersion.outputDir = fixture.path / "Build/PersistenceStaged";
        stagedSameVersion.cacheSourceDir = upgradeOutput;
        suite.Require(px::sdk::RunPackager(stagedSameVersion).Completed(),
                      "CLI-style staging retains its previous distribution source");
        const auto stagedManifest = px::sdk::detail::ParsePackageManifest(
            Read(stagedSameVersion.outputDir / "Package/manifest.json"));
        suite.Expect(stagedManifest.Valid() &&
                         stagedManifest.manifest.compatibleSaveFingerprints ==
                             std::vector<std::string>{priorFingerprint} &&
                         stagedManifest.manifest.legacyPersistenceReaders.size() == 1 &&
                         stagedManifest.manifest.legacyPersistenceReaders.front().secret ==
                             legacySecret,
                     "CLI-style staging carries both save ancestry and legacy keys");
        auto malformedManifest = nlohmann::json::parse(Read(manifestPath));
        malformedManifest["persistence"]["compatibleSaveFingerprints"] =
            nlohmann::json::array({
                repeatedManifest.manifest.packageFingerprint});
        suite.Expect(!px::sdk::detail::ParsePackageManifest(
                         malformedManifest.dump()).Valid(),
                     "a package cannot list its current fingerprint as retired save ancestry");
        Write(storyPath, originalStory);
        Write(projectPath, originalProject);
    });

    suite.Run("VideoContainersRemainSeekableInCompressedPackages", [&] {
        const auto videoPath = root / "Video/stream.mp4";
        const auto wmvPath = root / "Video/stream.wmv";
        std::uint64_t videoBytes = 32ull * 1024ull * 1024ull + 37ull;
        std::string requestedBytes;
#ifdef _WIN32
        char* requestedBuffer = nullptr;
        std::size_t requestedLength = 0;
        if (_dupenv_s(&requestedBuffer, &requestedLength,
                      "PRISMATIX_PACKAGER_RSS_BYTES") == 0 &&
            requestedBuffer) {
            requestedBytes.assign(requestedBuffer);
            std::free(requestedBuffer);
        }
#else
        if (const char* requested =
                std::getenv("PRISMATIX_PACKAGER_RSS_BYTES"))
            requestedBytes = requested;
#endif
        if (!requestedBytes.empty()) {
            const auto parsed = std::strtoull(requestedBytes.c_str(), nullptr, 10);
            if (parsed >= 2ull * 1024ull * 1024ull &&
                parsed <= 3ull * 1024ull * 1024ull * 1024ull)
                videoBytes = parsed;
        }
        WriteRepeated(videoPath, 'v', videoBytes);
        WriteRepeated(wmvPath, 'w', 4097);
        const std::uint64_t residentBefore = PeakResidentBytes();
        const auto videoOutput = fixture.path / "Build/StreamingVideo";
        auto videoRequest = FixtureRequest(root, videoOutput);
        videoRequest.requestId = "streaming-video-contract";
        videoRequest.encryption = false;
        videoRequest.compression = px::sdk::PackageCompression::Maximum;
        videoRequest.inputs.push_back(Input(root, "Video/stream.mp4"));
        videoRequest.inputs.push_back(Input(root, "Video/stream.wmv"));
        const auto result = px::sdk::RunPackager(videoRequest);
        suite.Require(result.Completed(),
                      "compressed package with a video container completes");
        px::io::Archive archive;
        suite.Require(archive.Open((videoOutput / "Content.pdx").string()),
                      "streaming video package mounts");
        const auto entry = std::ranges::find_if(
            archive.Entries(), [](const auto& candidate) {
                return candidate.name == "Video/stream.mp4";
            });
        suite.Require(entry != archive.Entries().end() && entry->flags == 0,
                      "Packager stores video without whole-entry compression");
        auto stream = archive.OpenStream("Video/stream.mp4");
        suite.Expect(stream && stream->Seek(-1, px::io::SeekOrigin::End) &&
                         stream->Tell() == videoBytes - 1,
                     "Packager output exposes a direct seekable video range");
        const auto wmvEntry = std::ranges::find_if(
            archive.Entries(), [](const auto& candidate) {
                return candidate.name == "Video/stream.wmv";
            });
        auto wmvStream = archive.OpenStream("Video/stream.wmv");
        suite.Expect(wmvEntry != archive.Entries().end() && wmvEntry->flags == 0 &&
                         wmvStream && wmvStream->Seek(-1, px::io::SeekOrigin::End) &&
                         wmvStream->Tell() == 4096,
                     "WMV remains directly seekable in compressed packages");

        {
            std::fstream changed(videoPath,
                                 std::ios::binary | std::ios::in | std::ios::out);
            suite.Require(changed.is_open(),
                          "large overlay source can be opened for a bounded edit");
            changed.seekp(static_cast<std::streamoff>(videoBytes / 2));
            changed.put('x');
            suite.Require(changed.good(),
                          "large overlay source edit is written");
        }
        const auto changedVideoOutput = fixture.path / "Build/ChangedStreamingVideo";
        auto changedVideoRequest = videoRequest;
        changedVideoRequest.requestId = "changed-streaming-video-contract";
        changedVideoRequest.outputDir = changedVideoOutput;
        changedVideoRequest.cacheSourceDir = videoOutput;
        for (auto& input : changedVideoRequest.inputs)
            if (input.uri == "Video/stream.mp4")
                input = Input(root, "Video/stream.mp4");
        suite.Require(px::sdk::RunPackager(changedVideoRequest).Completed(),
                      "large changed resource packages into a separate target");
        const auto videoOverlayPath = fixture.path / "large-video-overlay.pdx";
        const auto videoOverlay = px::io::CreatePackageOverlay(
            videoOutput, changedVideoOutput, videoOverlayPath);
        suite.Require(videoOverlay.created && videoOverlay.changedResources == 1,
                      "large changed resource produces one streamed overlay entry");
        const auto verifiedVideoOverlay =
            px::io::VerifyPackageOverlay(videoOutput, videoOverlayPath);
        suite.Require(verifiedVideoOverlay.valid &&
                          verifiedVideoOverlay.changedResources == 1,
                      "large streamed overlay passes independent verification");
        px::io::Archive videoOverlayArchive;
        suite.Require(videoOverlayArchive.Open(videoOverlayPath.string()),
                      "large overlay mounts as a PDX archive");
        auto changedVideoStream =
            videoOverlayArchive.OpenStream("Video/stream.mp4");
        std::uint8_t changedByte = 0;
        suite.Expect(changedVideoStream &&
                         changedVideoStream->Seek(
                             static_cast<std::int64_t>(videoBytes / 2),
                             px::io::SeekOrigin::Begin) &&
                         changedVideoStream->Read(&changedByte, 1) == 1 &&
                         changedByte == static_cast<std::uint8_t>('x') &&
                         changedVideoStream->PeakBufferedBytes() * 4u <
                             videoBytes,
                     "large overlay exposes the edited byte with bounded read buffering");

        const auto encryptedOutput = fixture.path / "Build/EncryptedStreamingVideo";
        auto encryptedRequest = FixtureRequest(root, encryptedOutput);
        encryptedRequest.requestId = "encrypted-streaming-video-contract";
        encryptedRequest.encryption = true;
        encryptedRequest.compression = px::sdk::PackageCompression::Maximum;
        encryptedRequest.inputs.push_back(Input(root, "Video/stream.mp4"));
        const auto encryptedResult = px::sdk::RunPackager(encryptedRequest);
        suite.Require(encryptedResult.Completed(),
                      "encrypted production package with video completes");
        suite.Expect(!std::filesystem::exists(encryptedOutput /
                                                "Package/entry-cache.json"),
                     "encrypted packages do not expose reusable archive byte ranges");
        const auto manifest = px::sdk::detail::ParsePackageManifest(
            Read(encryptedOutput / "Package/manifest.json"));
        suite.Require(manifest.Valid(),
                      "encrypted streaming package manifest should parse");
        const auto key = px::crypto::DeriveKey(manifest.manifest.archiveKey);
        px::io::Archive encryptedArchive;
        suite.Require(encryptedArchive.Open(
                          (encryptedOutput / "Content.pdx").string(), &key),
                      "encrypted streaming package should mount");
        const auto encryptedEntry = std::ranges::find_if(
            encryptedArchive.Entries(), [](const auto& candidate) {
                return candidate.name == "Video/stream.mp4";
            });
        suite.Require(encryptedEntry != encryptedArchive.Entries().end() &&
                          (encryptedEntry->flags & 0x05u) == 0x05u &&
                          encryptedEntry->chunkCount > 1,
                      "Packager must emit chunk-authenticated video records");
        auto encryptedStream =
            encryptedArchive.OpenStream("Video/stream.mp4");
        std::array<std::uint8_t, 16> tail{};
        suite.Expect(encryptedStream &&
                         encryptedStream->Seek(-16, px::io::SeekOrigin::End) &&
                         encryptedStream->Read(tail.data(), tail.size()) ==
                             tail.size() &&
                         std::ranges::all_of(tail, [](const std::uint8_t byte) {
                             return byte == static_cast<std::uint8_t>('v');
                         }) &&
                         encryptedStream->PeakBufferedBytes() * 4u < videoBytes,
                     "encrypted Packager video remains seekable with bounded memory");
        const std::uint64_t residentAfter = PeakResidentBytes();
        constexpr std::uint64_t memoryBudget = 64ull * 1024ull * 1024ull;
        suite.Expect(
            residentBefore == 0 || residentAfter == 0 ||
                residentAfter <= residentBefore + memoryBudget,
            "stream Packager peak RSS stays within a fixed budget independent of input size",
            "before=" + std::to_string(residentBefore) +
                " after=" + std::to_string(residentAfter) +
                " budget=" + std::to_string(memoryBudget));
    });

    suite.Run("CompressedMediaAvoidsRedundantWholeEntryCompression", [&] {
        WriteRepeated(root / "Assets/compressed-image.png", 'p', 16384);
        WriteRepeated(root / "Assets/compressed-voice.ogg", 'o', 16384);
        const auto output = fixture.path / "Build/PrecompressedMedia";
        auto mediaRequest = FixtureRequest(root, output);
        mediaRequest.requestId = "precompressed-media-contract";
        mediaRequest.encryption = false;
        mediaRequest.compression = px::sdk::PackageCompression::Maximum;
        mediaRequest.inputs.push_back(Input(root, "Assets/compressed-image.png"));
        mediaRequest.inputs.push_back(Input(root, "Assets/compressed-voice.ogg"));
        const auto result = px::sdk::RunPackager(mediaRequest);
        suite.Require(result.Completed(), "package with already-compressed media completes");
        px::io::Archive archive;
        suite.Require(archive.Open((output / "Content.pdx").string()),
                      "precompressed media archive mounts");
        for (const auto [uri, expected] : {
                 std::pair<std::string_view, std::uint8_t>{
                     "Assets/compressed-image.png", static_cast<std::uint8_t>('p')},
                 {"Assets/compressed-voice.ogg", static_cast<std::uint8_t>('o')}}) {
            const auto entry = std::ranges::find_if(
                archive.Entries(), [&](const auto& candidate) {
                    return candidate.name == uri;
                });
            const auto bytes = archive.Read(std::string(uri));
            suite.Expect(entry != archive.Entries().end() && entry->flags == 0 &&
                             bytes && bytes->size() == 16384 &&
                             std::ranges::all_of(*bytes, [&](const std::uint8_t byte) {
                                 return byte == expected;
                             }),
                         "PNG/OGG package entries remain directly readable without Zstd",
                         std::string(uri));
        }
    });

    suite.Run("DeferredMediaFingerprintIsCheckedByWriterAndShardReuse", [&] {
        const auto assetPath = root / "Assets/rin.png";
        const std::string originalAsset = Read(assetPath);
        const auto reusedOutput = fixture.path / "Build/DeferredMediaReuse";
        auto mediaRequest = FixtureRequest(root, reusedOutput);
        mediaRequest.requestId = "deferred-media-reuse";
        mediaRequest.encryption = false;
        mediaRequest.archiveShards = 4;
        suite.Require(px::sdk::RunPackager(mediaRequest).Completed(),
                      "the baseline media package completes");
        const auto baselineManifest = Read(reusedOutput / "Package/manifest.json");
        std::string changedAsset = originalAsset;
        changedAsset[0] = changedAsset[0] == 'x' ? 'y' : 'x';
        Write(assetPath, changedAsset);
        bool reachedStructureValidation = false;
        const auto reuseResult = px::sdk::RunPackager(
            mediaRequest, [&](const auto& event) {
                reachedStructureValidation |=
                    event.kind == px::sdk::PackageEventKind::Progress &&
                    event.phase == "validate-structure";
            });
        const auto freshOutput = fixture.path / "Build/DeferredMediaFresh";
        Write(freshOutput / "previous-success.txt", "preserved");
        mediaRequest.outputDir = freshOutput;
        mediaRequest.requestId = "deferred-media-writer";
        const auto freshResult = px::sdk::RunPackager(mediaRequest);
        Write(assetPath, originalAsset);
        suite.Expect(reachedStructureValidation &&
                         HasDiagnostic(reuseResult, "PXPKG1305") &&
                         Read(reusedOutput / "Package/manifest.json") ==
                             baselineManifest,
                     "whole-shard reuse rejects drifted media after deferred validation and preserves the old package");
        suite.Expect(HasDiagnostic(freshResult, "PXPKG1305") &&
                         Read(freshOutput / "previous-success.txt") ==
                             "preserved" &&
                         !std::filesystem::exists(freshOutput / "Content-00.pdx"),
                     "fresh stream writing rejects drifted media without replacing the old output");
    });

    suite.Run("TypeScriptInputIsNotMistakenForStreamingVideo", [&] {
        Write(root / "Scripts/extra.ts", "export const value = 1;\n");
        const auto failedOutput = fixture.path / "Build/TypeScriptFingerprint";
        Write(failedOutput / "previous-success.txt", "preserved");
        auto typed = FixtureRequest(root, failedOutput);
        typed.requestId = "typescript-fingerprint-contract";
        typed.inputs.push_back(Input(root, "Scripts/extra.ts"));
        typed.inputs.back().fingerprint = std::string(64, '0');
        const auto result = px::sdk::RunPackager(typed);
        suite.Expect(HasDiagnostic(result, "PXPKG1216") &&
                         Read(failedOutput / "previous-success.txt") ==
                             "preserved",
                     "ambiguous .ts inputs retain eager SHA validation");
    });

    suite.Run("CompressedEntryCacheReusesOnlyVerifiedMatchingBytes", [&] {
        const auto source = root / "Assets/cache-fixture.bin";
        WriteRepeated(source, 'c', 256 * 1024);
        const auto cachedOutput = fixture.path / "Build/EntryCache";
        auto cached = FixtureRequest(root, cachedOutput);
        cached.requestId = "entry-cache-contract";
        cached.encryption = false;
        cached.compression = px::sdk::PackageCompression::Fast;
        cached.inputs.push_back(Input(root, "Assets/cache-fixture.bin"));
        suite.Require(px::sdk::RunPackager(cached).Completed(),
                      "first package creates a compressed-entry cache");
        const auto cachePath = cachedOutput / "Package/entry-cache.json";
        suite.Require(std::filesystem::is_regular_file(cachePath),
                      "entry cache is shipped with the output it describes");
        const auto cacheDocument = nlohmann::json::parse(Read(cachePath));
        const auto& cacheRecords = cacheDocument.at("entries");
        const auto record = std::find_if(cacheRecords.begin(), cacheRecords.end(),
                                         [](const auto& item) {
                                             return item.at("uri") ==
                                                    "Assets/cache-fixture.bin";
                                         });
        suite.Require(record != cacheRecords.end(),
                      "compressible source has a reusable byte range");

        std::vector<px::sdk::PackageEvent> cacheEvents;
        suite.Require(px::sdk::RunPackager(cached, [&](const auto& event) {
                          cacheEvents.push_back(event);
                      }).Completed(), "second package reuses verified entries");
        const auto reuse = std::ranges::find_if(cacheEvents, [](const auto& event) {
            return event.phase == "archive-cache";
        });
        suite.Expect(reuse != cacheEvents.end() && reuse->current > 0,
                     "same-source package reports compressed cache reuse");
        auto staged = cached;
        staged.outputDir = fixture.path / "Build/EntryCacheStaged";
        staged.cacheSourceDir = cachedOutput;
        cacheEvents.clear();
        suite.Require(px::sdk::RunPackager(staged, [&](const auto& event) {
                          cacheEvents.push_back(event);
                      }).Completed(),
                      "staged frontend output can reuse an external published cache");
        const auto stagedReuse = std::ranges::find_if(
            cacheEvents, [](const auto& event) {
                return event.phase == "archive-cache";
            });
        suite.Expect(stagedReuse != cacheEvents.end() &&
                         stagedReuse->current > 0,
                     "external cache source is passed through to archive reuse");
        suite.Expect(std::filesystem::is_regular_file(
                         cachedOutput / "Content.pdx"),
                     "external cache source remains untouched");
        px::io::Archive archive;
        suite.Require(archive.Open((cachedOutput / "Content.pdx").string()),
                      "reused archive still mounts in the Player reader");
        const auto bytes = archive.Read("Assets/cache-fixture.bin");
        suite.Expect(bytes && bytes->size() == 256 * 1024 &&
                         std::ranges::all_of(*bytes, [](std::uint8_t value) {
                             return value == static_cast<std::uint8_t>('c');
                         }), "reused compressed bytes decode to the original source");
        archive.Close();

        const auto currentCache = nlohmann::json::parse(Read(cachePath));
        const auto corrupted = std::find_if(
            currentCache.at("entries").begin(),
            currentCache.at("entries").end(), [](const auto& item) {
                return item.at("uri") == "Assets/cache-fixture.bin";
            });
        suite.Require(corrupted != currentCache.at("entries").end(),
                      "replacement output keeps the cache record");
        const auto offset = corrupted->at("offset").get<std::uint64_t>();
        {
            std::fstream previous(cachedOutput / "Content.pdx",
                                  std::ios::binary | std::ios::in | std::ios::out);
            previous.seekg(static_cast<std::streamoff>(offset));
            char value = 0;
            previous.get(value);
            previous.seekp(static_cast<std::streamoff>(offset));
            previous.put(static_cast<char>(value ^ 0x01));
            suite.Require(static_cast<bool>(previous),
                          "test corrupts only the old cached byte range");
        }
        cacheEvents.clear();
        suite.Require(px::sdk::RunPackager(cached, [&](const auto& event) {
                          cacheEvents.push_back(event);
                      }).Completed(),
                      "tampered cache falls back to current source compression");
        const auto fallback = std::ranges::find_if(cacheEvents,
                                                    [](const auto& event) {
                                                        return event.phase ==
                                                               "archive-cache";
                                                    });
        suite.Expect(fallback != cacheEvents.end() &&
                         fallback->current < fallback->total,
                     "corrupt cached entry is not reused");
        const auto corruptShardReuse = std::ranges::find_if(
            cacheEvents, [](const auto& event) {
                return event.phase == "archive-shard-reuse";
            });
        suite.Expect(corruptShardReuse != cacheEvents.end() &&
                         corruptShardReuse->current == 0,
                     "tampered whole archive is never hard-linked into new output");
        suite.Require(archive.Open((cachedOutput / "Content.pdx").string()),
                      "fallback archive mounts after cache corruption");
        suite.Expect(archive.Read("Assets/cache-fixture.bin").has_value(),
                     "fallback archive contains the affected source");
        archive.Close();

        const auto priorArchive = px::sdk::ComputePackageFingerprint(
            cachedOutput / "Content.pdx");
        bool mutated = false;
        const auto changed = px::sdk::RunPackager(cached, [&](const auto& event) {
            if (!mutated && event.phase == "archive" &&
                event.message == "Writing Content.pdx") {
                WriteRepeated(source, 'x', 256 * 1024);
                mutated = true;
            }
        });
        WriteRepeated(source, 'c', 256 * 1024);
        suite.Expect(mutated && HasDiagnostic(changed, "PXPKG1305") &&
                         px::sdk::ComputePackageFingerprint(
                             cachedOutput / "Content.pdx") == priorArchive,
                     "cache reuse rechecks current source and preserves prior output");

        cached.compression = px::sdk::PackageCompression::Balanced;
        cacheEvents.clear();
        suite.Require(px::sdk::RunPackager(cached, [&](const auto& event) {
                          cacheEvents.push_back(event);
                      }).Completed(),
                      "different compression mode produces a fresh archive");
        const auto mismatch = std::ranges::find_if(cacheEvents,
                                                    [](const auto& event) {
                                                        return event.phase ==
                                                               "archive-cache";
                                                    });
        suite.Expect(mismatch != cacheEvents.end() && mismatch->current == 0,
                     "cache never crosses compression-level boundaries");
    });

    suite.Run("JsonIdentityRemainsCanonicalAfterBinaryProbe", [&] {
        const auto semanticPath = root / "Assets/semantic.json";
        Write(semanticPath, R"({"value":1,"label":"ok"})");
        auto first = FixtureRequest(root, fixture.path / "Build/JsonIdentityA");
        first.encryption = false;
        first.inputs.push_back(Input(root, "Assets/semantic.json"));
        suite.Require(px::sdk::RunPackager(first).Completed(),
                      "compact JSON package completes");
        const auto firstManifest = px::sdk::detail::ParsePackageManifest(
            Read(first.outputDir / "Package/manifest.json"));
        suite.Require(firstManifest.Valid(), "compact JSON manifest parses");

        Write(semanticPath, " { \"label\" : \"ok\", \"value\" : 1 } \n");
        auto second = FixtureRequest(root, fixture.path / "Build/JsonIdentityB");
        second.encryption = false;
        second.inputs.push_back(Input(root, "Assets/semantic.json"));
        suite.Require(px::sdk::RunPackager(second).Completed(),
                      "reformatted JSON package completes");
        const auto secondManifest = px::sdk::detail::ParsePackageManifest(
            Read(second.outputDir / "Package/manifest.json"));
        suite.Require(secondManifest.Valid(), "reformatted JSON manifest parses");
        suite.Expect(firstManifest.manifest.packageFingerprint ==
                         secondManifest.manifest.packageFingerprint,
                     "JSON identity still ignores formatting and object-key order");
    });

    suite.Run("LocaleTopology_StillChecksEachOperationKind", [&] {
        const auto localeIrPath = root / "Runtime/Locales/ja-JP/main.pxir";
        const auto original = Read(localeIrPath);
        auto localized = nlohmann::json::parse(original);
        localized["operations"][0]["kind"] = "narration";
        Write(localeIrPath, localized.dump());
        auto changed = FixtureRequest(root, fixture.path / "Build/LocaleTopologyMismatch");
        for (auto& input : changed.inputs) {
            if (input.uri == "Runtime/Locales/ja-JP/main.pxir") {
                input = Input(root, input.uri);
                break;
            }
        }
        std::vector<px::sdk::PackageEvent> changedEvents;
        const auto result = px::sdk::RunPackager(changed, [&](const auto& event) {
            changedEvents.push_back(event);
        });
        suite.Expect(!result.Completed() && HasDiagnostic(result, "PXPKG1263"),
                     "locale topology comparison retains kind identity without full parsed documents");
        suite.Expect(std::none_of(changedEvents.begin(), changedEvents.end(), [](const auto& event) {
                         return event.phase == "validate-primary-reuse";
                     }),
                     "changed locale Runtime IR falls back to independent primary validation");
        Write(localeIrPath, original);
    });

    suite.Run("LocaleSourceMap_StillChecksSourceLineBeforePrimaryReuse", [&] {
        const auto localeMapPath = root / "Runtime/Locales/ja-JP/main.pxmap";
        const auto original = Read(localeMapPath);
        auto localized = nlohmann::json::parse(original);
        localized["mappings"][0]["startLine"] = 2;
        localized["mappings"][0]["endLine"] = 2;
        Write(localeMapPath, localized.dump());
        auto changed = FixtureRequest(root, fixture.path / "Build/LocaleSourceLineMismatch");
        const auto result = px::sdk::RunPackager(changed);
        suite.Expect(!result.Completed() && HasDiagnostic(result, "PXPKG1261"),
                     "locale source-map source line is checked before identical primary documents can reuse validation");
        Write(localeMapPath, original);
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
              {"source", "Effects/dream-tone.pxeffect"}},
             {{"id", "node-tone"},
              {"source", "Effects/node-tone.pxeffect"}},
             {{"id", "screen-dream"},
              {"source", "Effects/screen-dream.pxeffect"}}});
        Write(effectRoot / "project.pxproject", project.dump());
        Write(effectRoot / "Effects/dream-tone.pxeffect", R"({
          "format":"PrismatiXEffect","schemaRevision":3,
          "id":"dream-tone","targetLayer":"stage",
          "shader":"Effects/dream-tone.frag.hlsl",
          "uniforms":[{"name":"amount","type":"number","slot":0,
            "default":0.5,"minimum":0,"maximum":1}]
        })");
        Write(effectRoot / "Effects/dream-tone.frag.hlsl", R"(
          cbuffer PrismatiXEffectContext : register(b0, space3) {
            float2 texelSize; float progress; float randomSeed;
            float4 parameters[8];
            float4 viewport;
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
        Write(effectRoot / "Effects/node-tone.pxeffect", R"({
          "format":"PrismatiXEffect","schemaRevision":3,
          "id":"node-tone","targetLayer":"node",
          "shader":"Effects/node-tone.frag.hlsl",
          "uniforms":[{"name":"tint","type":"color","slot":0,
            "default":[1,0.8,0.8,1],"minimum":0,"maximum":1}]
        })");
        Write(effectRoot / "Effects/node-tone.frag.hlsl", R"(
          cbuffer PrismatiXEffectContext : register(b0, space3) {
            float2 texelSize; float progress; float randomSeed;
            float4 parameters[8]; float4 viewport;
          };
          Texture2D nodeTexture : register(t0, space2);
          SamplerState nodeSampler : register(s0, space2);
          struct Input { float4 color : COLOR0; float2 uv : TEXCOORD0; };
          float4 main(Input input) : SV_Target {
            float4 color = nodeTexture.Sample(nodeSampler, input.uv) * input.color;
            return lerp(color, color * parameters[0], saturate(progress));
          }
        )");
        Write(effectRoot / "Effects/screen-dream.pxeffect", R"({
          "format":"PrismatiXEffect","schemaRevision":3,
          "id":"screen-dream","targetLayer":"transition",
          "shader":"Effects/screen-dream.frag.hlsl",
          "uniforms":[{"name":"softness","type":"number","slot":0,
            "default":0.1,"minimum":0,"maximum":1}]
        })");
        Write(effectRoot / "Effects/screen-dream.frag.hlsl", R"(
          cbuffer PrismatiXEffectContext : register(b0, space3) {
            float2 texelSize; float progress; float randomSeed;
            float4 parameters[8]; float4 viewport;
          };
          Texture2D outgoingTexture : register(t0, space2);
          Texture2D incomingTexture : register(t1, space2);
          SamplerState outgoingSampler : register(s0, space2);
          SamplerState incomingSampler : register(s1, space2);
          struct Input { float4 color : COLOR0; float2 uv : TEXCOORD0; };
          float4 main(Input input) : SV_Target {
            float edge = smoothstep(progress - parameters[0].x,
                                    progress + parameters[0].x, input.uv.x);
            float4 outgoing = outgoingTexture.Sample(outgoingSampler, input.uv);
            float4 incoming = incomingTexture.Sample(incomingSampler, input.uv);
            return lerp(incoming, outgoing, edge) * input.color;
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
        for (const auto& path : {
                 "Effects/node-tone.pxeffect",
                 "Effects/node-tone.frag.hlsl",
                 "Effects/screen-dream.pxeffect",
                 "Effects/screen-dream.frag.hlsl"})
            effectRequest.inputs.push_back(Input(effectRoot, path));

        const auto result = px::sdk::RunPackager(effectRequest);
        suite.Require(result.Completed(),
                      "valid custom HLSL compiles during packaging");
        const auto manifest = px::sdk::detail::ParsePackageManifest(
            Read(effectOutput / "Package/manifest.json"));
        suite.Require(manifest.Valid() &&
                          manifest.manifest.customEffects.size() == 3,
                      "Player parses reflected custom effect metadata");
        const auto effectIterator = std::ranges::find(
            manifest.manifest.customEffects, std::string("dream-tone"),
            &px::sdk::detail::PackageCustomEffect::id);
        suite.Require(effectIterator != manifest.manifest.customEffects.end(),
                      "stage custom effect remains in packaged metadata");
        const auto& effect = *effectIterator;
#ifdef __APPLE__
        constexpr std::size_t expectedArtifactCount = 2;
#else
        constexpr std::size_t expectedArtifactCount = 3;
#endif
        suite.Expect(effect.id == "dream-tone" && effect.targetLayer == "stage" &&
                         effect.schemaRevision == 3 &&
                         effect.samplerCount == 1 &&
                         effect.uniformBufferCount == 1 &&
                         effect.artifacts.size() == expectedArtifactCount &&
                         effect.uniforms.size() == 1,
                     "custom effect manifest has a fixed bounded resource schema");

        px::io::Archive archive;
        const auto key = px::crypto::DeriveKey(manifest.manifest.archiveKey);
        suite.Require(archive.Open((effectOutput / "Content.pdx").string(), &key),
                      "custom-effect package archive opens");
        const bool commonArtifacts =
            archive.Contains("Shaders/dream-tone/fragment.spv") &&
            archive.Contains("Shaders/dream-tone/fragment.msl");
#ifdef __APPLE__
        suite.Expect(commonArtifacts &&
                         !archive.Contains("Shaders/dream-tone/fragment.dxil"),
                     "macOS package carries reflected SPIR-V and native MSL without a fake DXIL artifact");
#else
        suite.Expect(commonArtifacts &&
                         archive.Contains("Shaders/dream-tone/fragment.dxil"),
                     "all release shader formats are packaged on DXC-capable hosts");
#endif
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
        for (const auto& packagedEffect : manifest.manifest.customEffects) {
            px::graphics::CustomEffectDescriptor runtimeEffect;
            runtimeEffect.id = packagedEffect.id;
            runtimeEffect.schemaRevision = packagedEffect.schemaRevision;
            runtimeEffect.targetLayer = packagedEffect.targetLayer;
            runtimeEffect.samplerCount = packagedEffect.samplerCount;
            runtimeEffect.uniformBufferCount = packagedEffect.uniformBufferCount;
            for (const auto& uniform : packagedEffect.uniforms)
                runtimeEffect.uniforms.push_back(
                    {uniform.name, uniform.type, uniform.slot,
                     uniform.defaultValue, uniform.minimum, uniform.maximum});
            for (const auto& artifact : packagedEffect.artifacts)
                runtimeEffect.artifacts.push_back(
                    {artifact.format, artifact.asset, artifact.fingerprint});
            runtimeConfig.customEffects.push_back(std::move(runtimeEffect));
        }

        px::Runtime runtime;
        suite.Require(runtime.Init(runtimeConfig),
                      "Runtime loads the packaged device-specific custom shader artifact");
        suite.Require(runtime.Renderer().HasCustomEffect("dream-tone"),
                      "custom effect is registered in the production compositor");
        suite.Expect(runtime.Renderer().HasCustomEffect("node-tone", "node") &&
                         runtime.Renderer().HasScreenEffect("screen-dream"),
                     "node and outgoing/incoming transition targets share the packaged compositor");
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
        const auto named = runtime.Renderer().ResolveCustomEffectParameters(
            "dream-tone", {{"amount", {0.8f}}});
        suite.Expect(named && (*named)[0][0] == 0.8f,
                     "declared custom effect parameters resolve by name");
        suite.Expect(
            !runtime.Renderer().ResolveCustomEffectParameters(
                "dream-tone", {{"amount", {1.1f}}}) &&
                !runtime.Renderer().ResolveCustomEffectParameters(
                    "dream-tone", {{"undeclared", {0.5f}}}),
            "named custom effect parameters enforce declared names and ranges");
        custom.customParameters = *defaults;
        const std::uint64_t effected = render(custom);
        suite.Expect(baseline != 0 && effected != 0 && baseline != effected,
                     "packaged custom effect visibly changes the Stage framebuffer");
        session.Stage().SetLayer("effected-layer", "Assets/rin.png", 32.0f,
                                 24.0f, 1.0f, 255, 2);
        suite.Require(session.Stage().SetNodeEffect(
                          "effected-layer", "node-tone", 0.75f,
                          {{"tint", {0.4f, 1.0f, 0.5f, 1.0f}}}),
                      "Stage graph accepts the packaged node effect schema");
        const auto nodeCheckpoint = session.Stage().CaptureState();
        suite.Require(session.Stage().RestoreState(nodeCheckpoint) &&
                          nodeCheckpoint.nodes.size() == 1 &&
                          nodeCheckpoint.nodes.front().effect == "node-tone",
                      "node effect parameters and deterministic seed restore through Stage state");

        suite.Require(runtime.Renderer().BeginFrame({210, 40, 50, 255}),
                      "custom transition captures an outgoing frame");
        runtime.Renderer().EndFrame();
        const px::graphics::CustomEffectNamedParameters transitionParameters{
            {"softness", {0.2f}}};
        const auto transitionHandle = runtime.Renderer().PlayScreenEffect(
            "screen-dream", 1.0f, &transitionParameters);
        suite.Require(transitionHandle != 0 &&
                          runtime.Renderer().BeginFrame({35, 70, 220, 255}),
                      "custom outgoing/incoming transition starts");
        runtime.Renderer().UpdateScreenEffects(0.5f);
        runtime.Renderer().EndFrame();
        SDL_Surface* transitionPixels = SDL_RenderReadPixels(
            runtime.Renderer().Handle(), nullptr);
        const auto transitionHash = SurfaceHash(transitionPixels);
        SDL_DestroySurface(transitionPixels);
        suite.Expect(transitionHash != 0 &&
                         runtime.Renderer().ScreenEffectPlaying(transitionHandle),
                     "custom transition renders both controlled textures at runtime");
        suite.Require(runtime.Renderer().LoadCustomEffects({}, runtime.VFS()) &&
                          !runtime.Renderer().HasCustomEffect("dream-tone") &&
                          !runtime.Renderer().HasScreenEffect("screen-dream"),
                      "custom effect reload releases Stage, node, and transition registrations");
        runtime.Shutdown();

        px::RuntimeConfig tamperedConfig = runtimeConfig;
        for (auto& artifact : tamperedConfig.customEffects.front().artifacts)
            artifact.fingerprint = std::string(64, '0');
        px::Runtime tamperedRuntime;
        suite.Expect(!tamperedRuntime.Init(tamperedConfig),
                     "Runtime rejects a shader artifact whose fingerprint metadata was tampered");
        tamperedRuntime.Shutdown();

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

    suite.Run("ValidationReportsProgressAndCanCancelBeforeStaging", [&] {
        const auto output = fixture.path / "Build/CancelledValidation";
        Write(output / "previous-success.txt", "preserved");
        auto validating = FixtureRequest(root, output);
        validating.requestId = "cancel-validation-contract";
        for (int index = 0; index < 300; ++index) {
            const std::string uri = "Assets/Validation/file-" +
                                    std::to_string(index) + ".bin";
            Write(root / uri, "data");
            validating.inputs.push_back(Input(root, uri));
        }
        bool requested = false;
        bool progressed = false;
        const auto result = px::sdk::RunPackager(
            validating, [&](const auto& event) {
                if (event.kind == px::sdk::PackageEventKind::Progress &&
                    event.phase == "validate" && event.current >= 256) {
                    progressed = true;
                    if (!requested) {
                        Write(validating.cancelFile, "cancel");
                        requested = true;
                    }
                }
            });
        std::error_code cleanupError;
        std::filesystem::remove(validating.cancelFile, cleanupError);
        suite.Expect(progressed && requested &&
                         result.exitCode == px::sdk::PackageExitCode::Cancelled &&
                         Read(output / "previous-success.txt") == "preserved" &&
                         !std::filesystem::exists(output / "Content.pdx"),
                     "long input validation reports progress and cancellation preserves output");
    });

    suite.Run("WriterRecheck_RejectsMutationBeforePromotion", [&] {
        const auto mutationOutput = fixture.path / "Build/Mutation";
        Write(mutationOutput / "previous-success.txt", "preserved");
        const auto assetPath = root / "Assets/rin.png";
        const auto originalAsset = Read(assetPath);
        auto mutated = FixtureRequest(root, mutationOutput);
        mutated.requestId = "writer-recheck-contract";
        bool injected = false;
        const auto result = px::sdk::RunPackager(
            mutated, [&](const auto& event) {
                if (!injected &&
                    event.kind == px::sdk::PackageEventKind::Progress &&
                    event.phase == "archive" &&
                    event.message == "Writing Content.pdx") {
                    Write(assetPath, "changed-after-identity");
                    injected = true;
                }
            });
        Write(assetPath, originalAsset);
        suite.Expect(!result.Completed() &&
                         HasDiagnostic(result, "PXPKG1305") &&
                         Read(mutationOutput / "previous-success.txt") ==
                             "preserved" &&
                         !std::filesystem::exists(mutationOutput /
                                                  "Content.pdx"),
                     "stream writer rechecks file bytes and keeps the prior output");
    });

    suite.Run("ValidationFailure_PreservesPriorSuccessfulOutput", [&] {
        const auto failedOutput = fixture.path / "Build/Failed";
        Write(failedOutput / "previous-success.txt", "preserved");
        auto failed = FixtureRequest(root, failedOutput);
        failed.requestId = "failure-contract";
        const auto projectInput = std::ranges::find_if(
            failed.inputs, [](const auto& input) {
                return input.uri == "project.pxproject";
            });
        suite.Require(projectInput != failed.inputs.end(),
                      "fixture includes an eagerly validated project document");
        projectInput->fingerprint = std::string(64, '0');
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
