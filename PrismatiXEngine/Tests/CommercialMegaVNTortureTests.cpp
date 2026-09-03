#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/Uuid.h"
#include "Engine/Graphics/AssetCache.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/IO/Archive.h"
#include "Engine/IO/VFS.h"
#include "Engine/Package/PackageManifest.h"
#include "Engine/Progression/SaveSystem.h"
#include "Engine/Runtime.h"
#include "Engine/SDK/Packager.h"
#include "Engine/SDK/RuntimeIr.h"
#include "Engine/Session/RuntimeSession.h"
#include "Tests/TestSupport/CanonicalUiFixture.h"
#include "Tests/TestSupport/TestHarness.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

namespace {

using Json = nlohmann::json;
constexpr std::size_t kDialogueCount = 20'000;
constexpr std::size_t kVoiceAssetCount = 1'024;
constexpr std::size_t kImageAssetCount = 1'024;
constexpr std::string_view kDocumentId = "commercial-mega-vn-main";
const std::vector<std::string> kLocales{"en-US", "zh-TW", "ja-JP", "fr-FR"};

void Write(const std::filesystem::path& path, const std::string_view value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
    if (!stream) throw std::runtime_error("could not write torture fixture");
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}

Json Operation(const std::size_t sequence, const std::string& kind,
               Json arguments, const std::string& suffix = {}) {
    const std::string id = "op-" + std::to_string(sequence) + suffix;
    return {{"operationId", id}, {"sourceId", "source-" + id},
            {"sourceLine", sequence + 1}, {"kind", kind}, {"text", ""},
            {"arguments", std::move(arguments)}};
}

Json BuildRuntimeIr(const std::string_view locale) {
    Json operations = Json::array();
    auto& values = operations.get_ref<Json::array_t&>();
    values.reserve(kDialogueCount + kDialogueCount / 100 + 8);
    std::size_t sequence = 0;
    values.push_back(Operation(sequence++, "scene", {{"title", "Mega Chapter"}}));
    for (std::size_t line = 0; line < kDialogueCount; ++line) {
        if (line % 1'000 == 0) {
            values.push_back(Operation(
                sequence++, "background",
                {{"asset", "Assets/Images/image-" +
                                   std::to_string(line % kImageAssetCount) + ".webp"},
                 {"duration", "0ms"}}));
        }
        Json args{{"speaker", "Character-" + std::to_string(line % 64)},
                  {"text", std::string(locale) + " commercial dialogue " +
                               std::to_string(line)},
                  {"speed", "0"}};
        if (line % 500 == 0) {
            args["voice"] = "Assets/Voice/voice-" +
                            std::to_string(line % kVoiceAssetCount) + ".ogg";
        }
        values.push_back(Operation(sequence++, "dialogue", std::move(args)));
        if (line % 500 == 499) {
            const std::string label = "resume-" + std::to_string(line);
            values.push_back(Operation(sequence++, "choiceOption",
                                       {{"text", std::string(locale) + " choice A"},
                                        {"target", label}}, "-choice-a"));
            values.push_back(Operation(sequence++, "choiceOption",
                                       {{"text", std::string(locale) + " choice B"},
                                        {"target", label}}, "-choice-b"));
            values.push_back(Operation(sequence++, "label", {{"target", label}},
                                       "-label"));
        }
    }
    values.push_back(Operation(sequence++, "endStory", Json::object()));
    return {{"format", "PrismatiXRuntimeIR"}, {"schemaRevision", 2},
            {"documentId", kDocumentId}, {"committedRevision", 7},
            {"operations", std::move(operations)}};
}

Json BuildSourceMap(const Json& runtime, const std::string& sourceUri) {
    Json mappings = Json::array();
    auto& values = mappings.get_ref<Json::array_t&>();
    values.reserve(runtime.at("operations").size());
    for (const auto& operation : runtime.at("operations")) {
        const auto line = operation.at("sourceLine");
        values.push_back({{"operationId", operation.at("operationId")},
                          {"sourceId", operation.at("sourceId")},
                          {"sourceUri", sourceUri}, {"startLine", line},
                          {"startColumn", 1}, {"endLine", line},
                          {"endColumn", 2}});
    }
    return {{"format", "PrismatiXSourceMap"}, {"schemaRevision", 2},
            {"documentId", kDocumentId}, {"mappings", std::move(mappings)}};
}

std::string UiDocument() {
    return px::test::CanonicalUiFixtureText(R"({
      "format":"PrismatiXUIScene","schemaRevision":1,
      "id":"77777777-7777-4777-8777-777777777777","revision":1,
      "name":"Commercial HUD","width":1280,"height":720,
      "rootId":"88888888-8888-4888-8888-888888888888",
      "nodes":[{"id":"88888888-8888-4888-8888-888888888888",
        "parentId":null,"order":0,"kind":"control","name":"Root",
        "visible":true,"locked":false,
        "layout":{"mode":"free","x":0,"y":0,"width":1280,"height":720,
          "anchorX":0,"anchorY":0,"pivotX":0,"pivotY":0,"margin":0,
          "alignment":"start","sizeRule":"fixed"},
        "content":{"text":"","assetId":null},
        "appearance":{"backgroundColor":"#101018","textColor":"#FFFFFF",
          "opacity":1,"styleToken":null,"hoverBackgroundColor":null,
          "focusColor":null,"disabledOpacity":0.5},
        "interaction":{"onClick":null},
        "accessibility":{"label":"","role":"presentation"}}],"theme":[]
    })");
}

px::sdk::PackageInput Input(const std::filesystem::path& root,
                            const std::string& uri) {
    const auto path = root / std::filesystem::path(uri);
    return {uri, px::sdk::ComputePackageFingerprint(path),
            std::filesystem::file_size(path)};
}

struct FixturePaths {
    std::filesystem::path root;
    std::filesystem::path output;
    std::vector<std::string> inputs;
    std::string canonicalRuntime;
};

FixturePaths BuildFixture(const std::filesystem::path& base) {
    FixturePaths fixture{base / "Project", base / "Build" / "CommercialMegaVN"};
    Json canonical = BuildRuntimeIr(kLocales.front());
    fixture.canonicalRuntime = canonical.dump();
    const std::string canonicalSource = "Story/main-en-US.pxstory";
    Write(fixture.root / "Content/Runtime/main.pxir", fixture.canonicalRuntime);
    Write(fixture.root / "Content/Runtime/main.pxmap",
          BuildSourceMap(canonical, canonicalSource).dump());
    fixture.inputs = {"Content/Runtime/main.pxir", "Content/Runtime/main.pxmap"};

    Json storySources = Json::object();
    for (const auto& locale : kLocales) {
        Json localized = BuildRuntimeIr(locale);
        const std::string storyPath = "Story/main-" + locale + ".pxstory";
        const std::string runtimePath = "Runtime/Locales/" + locale + "/main.pxir";
        const std::string mapPath = "Runtime/Locales/" + locale + "/main.pxmap";
        const std::string localePath = "Content/Localization/" + locale + ".json";
        Write(fixture.root / storyPath,
              "# synthetic commercial story source for " + locale + "\n");
        Write(fixture.root / runtimePath, localized.dump());
        Write(fixture.root / mapPath, BuildSourceMap(localized, storyPath).dump());
        Json strings = Json::object();
        for (std::size_t index = 0; index < 1'000; ++index)
            strings["ui." + std::to_string(index)] =
                locale + " localized string " + std::to_string(index);
        Write(fixture.root / localePath,
              Json{{"format", "PrismatiXLocale"}, {"schemaRevision", 2},
                   {"locale", locale}, {"strings", std::move(strings)}}.dump());
        storySources[locale] = storyPath;
        fixture.inputs.insert(fixture.inputs.end(),
                              {storyPath, runtimePath, mapPath, localePath});
    }

    Write(fixture.root / "Story/story.pxindex",
          Json{{"format", "PrismatiXStoryIndex"}, {"schemaRevision", 2},
               {"id", "main"}, {"entryScene", "main"},
               {"chapters", Json::array({{{"id", "chapter-1"},
                                            {"title", "Mega Chapter"},
                                            {"scenes", Json::array({"main"})}}})},
               {"scenes", Json::array({{{"id", "main"},
                                          {"sources", std::move(storySources)}}})}}
              .dump());
    Write(fixture.root / "Content/game.pxgame",
          Json{{"format", "PrismatiXGame"}, {"schemaRevision", 2},
               {"variables", Json::array()}, {"gallery", Json::array()},
               {"unlockables", Json::array()}}
              .dump());
    Write(fixture.root / "Content/UI/HUD.pxui", UiDocument());
    Write(fixture.root / "Content/Migrations/v1-to-v2.pxsave-migration",
          R"({"format":"PrismatiXSaveMigration","schemaRevision":2,
          "id":"commercial-v1-to-v2",
          "from":{"contentVersion":"commercial-v1","saveVersion":1},
          "to":{"contentVersion":"commercial-v2","saveVersion":2},
          "anchor":{"policy":"preserve"},
          "operations":[{"op":"renameVariable","from":"route","to":"routeV2"}]})");
    fixture.inputs.insert(fixture.inputs.end(),
                          {"Story/story.pxindex", "Content/game.pxgame",
                           "Content/UI/HUD.pxui",
                           "Content/Migrations/v1-to-v2.pxsave-migration"});

    Json assets = Json::array();
    for (std::size_t index = 0; index < kVoiceAssetCount; ++index) {
        const std::string uri = "Assets/Voice/voice-" + std::to_string(index) + ".ogg";
        Write(fixture.root / uri, "synthetic-voice-" + std::to_string(index));
        assets.push_back({{"id", px::Uuid::FromName(uri).ToString()},
                          {"name", "Voice " + std::to_string(index)},
                          {"kind", "audio"}, {"source", uri}});
        fixture.inputs.push_back(uri);
    }
    for (std::size_t index = 0; index < kImageAssetCount; ++index) {
        const std::string uri = "Assets/Images/image-" + std::to_string(index) + ".webp";
        Write(fixture.root / uri, "synthetic-image-" + std::to_string(index));
        assets.push_back({{"id", px::Uuid::FromName(uri).ToString()},
                          {"name", "Image " + std::to_string(index)},
                          {"kind", "image"}, {"source", uri}});
        fixture.inputs.push_back(uri);
    }

    const Json migration = {{"id", "commercial-v1-to-v2"},
                            {"from", {{"contentVersion", "commercial-v1"},
                                      {"saveVersion", 1}}},
                            {"to", {{"contentVersion", "commercial-v2"},
                                    {"saveVersion", 2}}},
                            {"asset", "Content/Migrations/v1-to-v2.pxsave-migration"}};
    Write(fixture.root / "project.pxproject",
          Json{{"format", "PrismatiXProject"}, {"schemaRevision", 2},
               {"id", "commercial-mega-vn"}, {"name", "CommercialMegaVN"},
               {"version", "2.0.0"}, {"contentVersion", "commercial-v2"},
               {"saveVersion", 2}, {"graphicsTier", "basic"},
               {"saveMigrations", Json::array({migration})},
               {"resolution", {{"width", 1280}, {"height", 720}}},
               {"entry", {{"story", "main"}, {"ui", "hud"}}},
               {"defaultLocale", "en-US"}, {"supportedLocales", kLocales},
               {"storyIndex", "Story/story.pxindex"},
               {"gameCatalog", "Content/game.pxgame"},
               {"extensions", Json::array()},
               {"uiEntryPoints", {{"hud", "Content/UI/HUD.pxui"}}},
               {"assets", std::move(assets)}, {"characters", Json::array()}}
              .dump());
    fixture.inputs.push_back("project.pxproject");

#ifdef _WIN32
    Write(fixture.root / "Tools/PrismatiXPlayer.exe", "synthetic-player");
    Write(fixture.root / "Tools/runtime-fixture.dll", "synthetic-runtime");
#elif defined(__APPLE__)
    Write(fixture.root / "Tools/PrismatiXPlayer", "synthetic-player");
    Write(fixture.root / "Tools/libruntime-fixture.dylib", "synthetic-runtime");
#else
    Write(fixture.root / "Tools/PrismatiXPlayer", "synthetic-player");
    Write(fixture.root / "Tools/libruntime-fixture.so", "synthetic-runtime");
#endif
    std::ranges::sort(fixture.inputs);
    fixture.inputs.erase(std::unique(fixture.inputs.begin(), fixture.inputs.end()),
                         fixture.inputs.end());
    return fixture;
}

struct RuntimeFixture {
    px::io::VFS vfs;
    px::audio::AudioEngine audio{vfs};
    px::graphics::AssetCache assets{nullptr, vfs};
    px::graphics::Renderer2D renderer{nullptr, assets};
    px::RuntimeSession runtime{{vfs, audio, renderer, assets}};
};

px::progress::SaveSnapshot SaveFromState(
    const px::RuntimeSession::GameState& state, const px::vn::VM& vm) {
    px::progress::SaveSnapshot save;
    save.gameId = "commercial-mega-vn";
    save.packageFingerprint = std::string(64, 'a');
    save.contentVersion = "commercial-v1";
    save.saveVersion = 1;
    save.anchor = {vm.CurrentDocumentId(), vm.CurrentSourceId(),
                   vm.CurrentOperationId()};
    save.scriptPath = state.vm.scriptPath;
    save.pc = state.vm.pc;
    save.chapter = state.vm.chapter;
    save.vm = state.vm;
    save.dialogue = state.dialogue;
    save.variables = state.variables;
    save.typedVariables = state.typedVariables;
    save.stage = state.stage;
    save.audio = state.audio;
    save.backlog = state.backlog;
    save.routes = state.routes;
    save.timelines = state.timelines;
    save.animationClips = state.animationClips;
    save.playtimeMs = state.playtimeMs;
    save.timestamp = 1'700'000'000;
    return save;
}

}  // namespace

int main() {
    px::test::Suite suite("CommercialMegaVNTorture");
    px::test::TempDirectory temp("commercial-mega-vn");
    FixturePaths fixture;
    bool packaged = false;

    suite.Run("ValidateCompilePackageStartup", [&] {
        const auto start = std::chrono::steady_clock::now();
        fixture = BuildFixture(temp.path);
        const auto parsed = px::sdk::ParseRuntimeIr(fixture.canonicalRuntime);
        suite.Require(parsed.Valid(), "commercial Runtime IR validates");
        suite.Expect(parsed.document.operations.size() > kDialogueCount,
                     "fixture contains dialogue, labels, choices, and visual operations");

        px::sdk::PackageRequest request;
        request.requestId = "commercial-mega-vn-torture";
        request.gameId = "commercial-mega-vn";
        request.projectRoot = fixture.root;
        request.outputDir = fixture.output;
#ifdef _WIN32
        request.playerExecutable = fixture.root / "Tools/PrismatiXPlayer.exe";
#else
        request.playerExecutable = fixture.root / "Tools/PrismatiXPlayer";
#endif
        request.title = "CommercialMegaVN";
        request.width = 1280;
        request.height = 720;
        request.startScript = "Content/Runtime/main.pxir";
        request.sourceMap = "Content/Runtime/main.pxmap";
        request.startRoute = "hud";
        request.routes = {{"hud", "Content/UI/HUD.pxui"}};
        request.saveMigrations = {{"commercial-v1-to-v2", "commercial-v1", 1,
                                   "commercial-v2", 2,
                                   "Content/Migrations/v1-to-v2.pxsave-migration"}};
        request.contentVersion = "commercial-v2";
        request.saveVersion = 2;
        request.compression = px::sdk::PackageCompression::Fast;
        request.cancelFile = fixture.root / ".cancel";
        request.inputs.reserve(fixture.inputs.size());
        for (const auto& uri : fixture.inputs)
            request.inputs.push_back(Input(fixture.root, uri));
        const auto result = px::sdk::RunPackager(request);
        suite.Require(result.Completed(), "commercial project validates and packages");
        packaged = result.Completed();

        const auto manifest = px::sdk::detail::ParsePackageManifest(
            Read(fixture.output / "Package/manifest.json"));
        suite.Require(manifest.Valid(), "packaged startup manifest validates");
        px::io::VFS playerVfs;
        suite.Require(playerVfs.MountArchive(
                          (fixture.output / "Content.pdx").string()),
                      "packaged commercial archive mounts");
        px::audio::AudioEngine audio(playerVfs);
        px::graphics::AssetCache assets(nullptr, playerVfs);
        px::graphics::Renderer2D renderer(nullptr, assets);
        px::RuntimeSession runtime({playerVfs, audio, renderer, assets});
        suite.Expect(runtime.StartRuntimeIr(manifest.manifest.startRuntimeIr),
                     "packaged commercial entry starts through RuntimeSession");
        const auto elapsed = std::chrono::steady_clock::now() - start;
        std::cout << "CommercialMegaVN validate/compile/package/startup: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                  << " ms; operations=" << parsed.document.operations.size()
                  << "; assets=" << (kVoiceAssetCount + kImageAssetCount)
                  << "; locales=" << kLocales.size() << '\n';
        suite.Expect(elapsed < std::chrono::seconds(120),
                     "commercial pipeline stays within the stress budget");
    });

    suite.Run("AutoSkipBacklogRollbackAndSeek", [&] {
        RuntimeFixture live;
        live.vfs.MountDirectory(fixture.root.string());
        suite.Require(live.runtime.StartRuntimeIr("Content/Runtime/main.pxir"),
                      "large Runtime IR starts");
        std::optional<px::RuntimeSession::GameState> rollback;
        std::uint64_t now = 0;
        std::size_t cycles = 0;
        std::size_t choices = 0;
        const auto start = std::chrono::steady_clock::now();
        while (live.runtime.VM().State() != px::vn::VMState::Finished &&
               cycles < kDialogueCount * 3) {
            if (live.runtime.VM().State() == px::vn::VMState::WaitingChoice) {
                live.runtime.SelectChoice(static_cast<int>(choices++ % 2));
            } else if (live.runtime.VM().State() == px::vn::VMState::WaitingClick) {
                live.runtime.Dialogue().ShowAll();
                live.runtime.Advance();
            } else {
                now += 16;
                live.runtime.Update(now, 0.016f);
            }
            if (++cycles == kDialogueCount / 2)
                rollback = live.runtime.CaptureState(now);
        }
        suite.Require(live.runtime.VM().State() == px::vn::VMState::Finished,
                      "long auto/skip traversal reaches EOF");
        suite.Expect(choices == kDialogueCount / 500,
                     "all large choice blocks execute");
        suite.Expect(live.runtime.Backlog().Size() == 500,
                     "backlog remains at its production memory bound");
        suite.Require(rollback.has_value(), "mid-run rollback snapshot exists");
        const int rollbackPc = rollback->vm.pc;
        suite.Require(static_cast<bool>(live.runtime.RestoreState(*rollback, now)),
                      "large rollback state restores transactionally");
        suite.Expect(live.runtime.VM().ProgramCounter() == rollbackPc &&
                         live.runtime.Backlog().Size() <= 500,
                     "rollback restores execution and bounded backlog");
        suite.Expect(live.runtime.SeekRuntimeIrOperation(
                         fixture.canonicalRuntime, "Content/Runtime/main.pxir", 100) ==
                         px::vn::ProgramSeekStatus::Applied,
                     "large compiled story seeks deterministically");
        const auto elapsed = std::chrono::steady_clock::now() - start;
        std::cout << "CommercialMegaVN auto/skip + rollback/seek: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                  << " ms\n";
    });

    suite.Run("MassSaveLoadAndMigration", [&] {
        RuntimeFixture live;
        live.vfs.MountDirectory(fixture.root.string());
        suite.Require(live.runtime.StartRuntimeIr("Content/Runtime/main.pxir"),
                      "save fixture starts");
        for (int line = 0; line < 600; ++line) {
            if (live.runtime.VM().State() == px::vn::VMState::WaitingChoice)
                live.runtime.SelectChoice(0);
            if (live.runtime.VM().State() == px::vn::VMState::WaitingClick) {
                live.runtime.Dialogue().ShowAll();
                live.runtime.Advance();
            }
        }
        auto save = SaveFromState(live.runtime.CaptureState(123'456),
                                  live.runtime.VM());
        save.typedVariables["route"] = px::vn::Value("alpha");
        px::progress::SaveSystem saves;
        const auto saveRoot = temp.path / "Saves";
        saves.Configure(saveRoot.string(), nullptr);
        const auto start = std::chrono::steady_clock::now();
        for (int iteration = 0; iteration < 256; ++iteration) {
            const int slot = iteration % 32;
            save.timestamp = static_cast<std::uint64_t>(iteration + 1);
            suite.Require(saves.Save(slot, save), "bulk save write succeeds");
            const auto loaded = saves.Load(slot);
            suite.Require(loaded && loaded->vm.pc == save.vm.pc &&
                              loaded->backlog.size() == save.backlog.size(),
                          "bulk save load preserves large runtime state");
        }
        const std::vector<px::progress::SaveMigrationDescriptor> migrations{{
            "commercial-v1-to-v2", "commercial-v1", 1, "commercial-v2", 2,
            "Content/Migrations/v1-to-v2.pxsave-migration"}};
        const auto migrated = px::progress::MigrateSaveSnapshot(
            save, {"commercial-mega-vn", std::string(64, 'b'),
                   "commercial-v2", 2}, migrations,
            [&](const std::string_view path) -> std::optional<std::string> {
                const auto text = live.vfs.ReadText(path);
                return text ? std::optional<std::string>(*text) : std::nullopt;
            });
        suite.Expect(migrated && migrated.Value().typedVariables.contains("routeV2") &&
                         !migrated.Value().typedVariables.contains("route"),
                     "commercial save migration is deterministic");
        const auto elapsed = std::chrono::steady_clock::now() - start;
        std::cout << "CommercialMegaVN 256 save/load cycles + migration: "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                  << " ms\n";
    });

    suite.Run("LocaleSwitchResourceLifetimeAndSoak", [&] {
        suite.Require(packaged, "packaged fixture is available");
        RuntimeFixture live;
        suite.Require(live.vfs.MountArchive(
                          (fixture.output / "Content.pdx").string()),
                      "commercial package remounts for lifetime test");
        suite.Require(live.runtime.StartRuntimeIr("Runtime/Locales/en-US/main.pxir"),
                      "default localized runtime starts");
        const auto original = live.runtime.CaptureState();
        const std::string stableSource = live.runtime.VM().CurrentSourceId();
        for (std::size_t index = 1; index < kLocales.size(); ++index) {
            const std::string path = "Runtime/Locales/" + kLocales[index] + "/main.pxir";
            auto program = live.runtime.PrepareRuntimeIr(path);
            suite.Require(program &&
                              program->code.size() == original.runtimeProgram->code.size(),
                          "locale program retains stable topology");
            auto localized = original;
            localized.runtimeProgram = std::move(program);
            localized.vm.scriptPath = path;
            suite.Require(static_cast<bool>(live.runtime.RestoreState(localized)),
                          "locale program switches on captured runtime state");
            live.renderer.SetTextLocale(kLocales[index]);
            suite.Expect(live.renderer.TextLocale() == kLocales[index] &&
                             live.runtime.VM().CurrentSourceId() == stableSource,
                         "locale switch preserves stable narrative identity");
        }

        for (std::size_t index = 0; index < 10'000; ++index) {
            const std::string path = "Assets/Images/image-" +
                                     std::to_string(index % kImageAssetCount) + ".webp";
            auto stream = live.vfs.Open(path);
            suite.Require(stream && stream->Size() > 0,
                          "streaming asset handle opens during lifetime churn");
            std::array<std::uint8_t, 4> bytes{};
            suite.Expect(stream->Read(bytes.data(), bytes.size()) == bytes.size(),
                         "streaming asset handle reads a bounded prefix");
        }

        live.runtime.Stage().SetParticleEmitter(
            "soak-rain", {.preset = px::vn::ParticlePreset::Rain, .seed = 99,
                          .rate = 120.0f, .maxParticles = 384});
        constexpr int acceleratedEightHours = 8 * 60 * 60 * 4;
        for (int update = 0; update < acceleratedEightHours; ++update)
            live.runtime.Stage().Update(0.25f);
        const auto state = live.runtime.Stage().CaptureState();
        suite.Expect(state.particleEmitters.size() == 1 &&
                         live.runtime.Stage().SampleParticles("soak-rain", 1280, 720)
                                 .size() <= 384,
                     "eight-hour native weather soak remains bounded and restorable");
    });

    return suite.Finish();
}
