// The native half of scripts/release-journey.mjs: `write-v1` writes player
// saves, profile and config from the packaged V1 release; `verify-v2` reads
// them from the packaged V2 release and migrates the slot through its
// packaged migration. It reads only packaged artifacts.

#include "Engine/IO/Archive.h"
#include "Engine/Progression/GameSettings.h"
#include "Engine/Progression/GlobalProfile.h"
#include "Engine/Progression/GlobalProfileStore.h"
#include "Engine/Progression/Persist.h"
#include "Engine/Progression/SaveSystem.h"
#include "Engine/Package/PackageManifest.h"
#include "Tests/TestSupport/TestHarness.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    return bytes.str();
}

px::sdk::detail::PackageManifest ReadManifest(
    px::test::Suite& suite, const std::filesystem::path& release) {
    const auto parsed = px::sdk::detail::ParsePackageManifest(
        Read(release / "Package/manifest.json"));
    suite.Require(parsed.Valid(), "packaged release manifest is valid");
    return parsed.manifest;
}

std::vector<px::crypto::Key> FallbackKeys(
    const px::sdk::detail::PackageManifest& manifest) {
    std::vector<px::crypto::Key> keys;
    keys.reserve(manifest.legacyPersistenceReaders.size());
    for (const auto& reader : manifest.legacyPersistenceReaders)
        keys.push_back(
            px::progress::DeriveLegacyPersistenceKey(reader.secret));
    return keys;
}

std::optional<std::string> ReadArchiveText(
    const px::io::Archive& archive, const std::string_view path) {
    const auto bytes = archive.Read(path);
    if (!bytes) return std::nullopt;
    return std::string(bytes->begin(), bytes->end());
}

px::io::Archive OpenArchive(px::test::Suite& suite,
                            const std::filesystem::path& release,
                            const px::sdk::detail::PackageManifest& manifest) {
    px::io::Archive archive;
    std::optional<px::crypto::Key> archiveKey;
    if (manifest.encrypted) archiveKey = px::crypto::DeriveKey(manifest.archiveKey);
    suite.Require(
        archive.Open((release / "Content.pdx").string(),
                     archiveKey ? &*archiveKey : nullptr),
        "packaged release archive opens without repository files");
    return archive;
}

bool WriteVersionOne(px::test::Suite& suite,
                     const std::filesystem::path& release) {
    const auto manifest = ReadManifest(suite, release);
    suite.Require(manifest.gameId == "prismatix-release-journey" &&
                      manifest.contentVersion == "journey-v1" &&
                      manifest.saveVersion == 1 &&
                      !manifest.persistenceDataId.empty(),
                  "version-one package publishes a stable persistence identity");
    const auto key = px::progress::DeriveGameDataPersistenceKey(
        manifest.persistenceDataId);
    const auto saveRoot = release / "Save";
    std::filesystem::create_directories(saveRoot);

    px::progress::GlobalProfile profile;
    profile.MarkSeen("main.long.page.520");
    profile.MarkChoiceSeen("main.choice.long");
    profile.UnlockCG("evening-ending");
    px::VariantArray rememberedRoutes;
    rememberedRoutes.emplace_back("walk");
    rememberedRoutes.emplace_back("long-night");
    px::VariantObject releaseState;
    releaseState.emplace("route", px::Variant("long-night"));
    releaseState.emplace("rememberedRoutes",
                         px::Variant(std::move(rememberedRoutes)));
    releaseState.emplace("version", px::Variant(std::int64_t{1}));
    profile.SetVariable("journey.release",
                        px::Variant(std::move(releaseState)));
    suite.Require(px::progress::SaveGlobalProfile(
                      profile, (saveRoot / "profile.dat").string(), &key),
                  "version-one package writes profile and composite state");

    px::progress::GameSettings settings;
    settings.language = "ja-JP";
    settings.textSpeedMs = 17;
    settings.autoWaitMs = 925;
    settings.reducedMotion = true;
    suite.Require(settings.Save((saveRoot / "config.dat").string(), &key),
                  "version-one package writes player config");

    const auto archive = OpenArchive(suite, release, manifest);
    const auto runtimeText = ReadArchiveText(archive, manifest.startRuntimeIr);
    suite.Require(runtimeText.has_value(),
                  "version-one runtime program comes from packaged artifacts");
    const auto runtime = nlohmann::json::parse(*runtimeText, nullptr, false);
    suite.Require(runtime.is_object() && runtime.contains("operations") &&
                      runtime["operations"].is_array() &&
                      !runtime["operations"].empty(),
                  "packaged authoring output contains executable Runtime IR");
    const auto& first = runtime["operations"].front();

    px::progress::SaveSnapshot snapshot;
    snapshot.gameId = manifest.gameId;
    snapshot.packageFingerprint = manifest.packageFingerprint;
    snapshot.contentVersion = manifest.contentVersion;
    snapshot.saveVersion = manifest.saveVersion;
    snapshot.anchor = {
        runtime.value("documentId", std::string{}),
        first.value("sourceId", std::string{}),
        first.value("operationId", std::string{}),
    };
    snapshot.scriptPath = manifest.startRuntimeIr;
    snapshot.vm.scriptPath = manifest.startRuntimeIr;
    snapshot.chapter = "chapter-evening";
    snapshot.routes.stack = {"hud"};
    snapshot.typedVariables["walkChoice"] = px::Variant("long-night");
    snapshot.typedVariables["endingUnlocked"] = px::Variant(true);
    snapshot.timestamp = 1'800'000'000;
    snapshot.playtimeMs = 9'876'543;
    px::progress::SaveSystem saves;
    saves.Configure(saveRoot.string(), &key);
    suite.Require(saves.Save(7, snapshot),
                  "version-one package writes an encrypted slot");

    const auto wrongKey =
        px::progress::DeriveLegacyPersistenceKey("deliberately-wrong-key");
    px::progress::GlobalProfile protectedProfile;
    protectedProfile.MarkSeen("must-survive-upgrade-failure");
    suite.Require(px::progress::SaveGlobalProfile(
                      protectedProfile,
                      (saveRoot / "protected-wrong-key.dat").string(),
                      &wrongKey),
                  "version-one writes the wrong-key preservation fixture");
    return true;
}

bool VerifyVersionTwo(px::test::Suite& suite,
                      const std::filesystem::path& release) {
    const auto manifest = ReadManifest(suite, release);
    suite.Require(manifest.gameId == "prismatix-release-journey" &&
                      manifest.contentVersion == "journey-v2" &&
                      manifest.saveVersion == 2 &&
                      manifest.saveMigrations.size() == 1,
                  "version-two package declares the fixture migration");
    const auto key = px::progress::DeriveGameDataPersistenceKey(
        manifest.persistenceDataId);
    const auto fallbackKeys = FallbackKeys(manifest);
    const auto saveRoot = release / "Save";

    px::progress::GlobalProfile profile;
    suite.Require(
        px::progress::LoadGlobalProfileWithFallback(
            profile, (saveRoot / "profile.dat").string(), &key,
            fallbackKeys) == px::progress::PersistLoadStatus::Loaded &&
            profile.HasSeen("main.long.page.520") &&
            profile.HasChoiceSeen("main.choice.long") &&
            profile.CGUnlocked("evening-ending") &&
            profile.Variable("journey.release") != nullptr,
        "version-two reads version-one seen, unlock, and composite profile data");
    const auto* releaseState = profile.Variable("journey.release")->AsObject();
    suite.Expect(releaseState && releaseState->contains("rememberedRoutes") &&
                     releaseState->at("rememberedRoutes").AsArray() &&
                     releaseState->at("rememberedRoutes").AsArray()->size() == 2,
                 "composite profile state survives the package upgrade");

    px::progress::GameSettings settings;
    suite.Require(
        settings.LoadWithFallback((saveRoot / "config.dat").string(), &key,
                                  fallbackKeys) ==
                px::progress::PersistLoadStatus::Loaded &&
            settings.language == "ja-JP" && settings.textSpeedMs == 17 &&
            settings.autoWaitMs == 925 && settings.reducedMotion,
        "version-two reads version-one config unchanged");

    px::progress::SaveSystem saves;
    saves.Configure(saveRoot.string(), &key, fallbackKeys);
    const auto loaded = saves.Load(7);
    suite.Require(loaded.has_value() &&
                      loaded->contentVersion == "journey-v1" &&
                      loaded->saveVersion == 1 &&
                      loaded->packageFingerprint != manifest.packageFingerprint,
                  "version-two reads the exact version-one slot before migration");

    const auto archive = OpenArchive(suite, release, manifest);
    suite.Expect(
        archive.Contains("Assets/Locales/zh-TW/greeting-v2.wav") &&
            archive.Contains("Assets/Locales/ja-JP/greeting-v2.wav") &&
            archive.Contains("Assets/ending-cg.png") &&
            archive.Contains("Assets/evening-room.png") &&
            archive.Contains("ASSET_PROVENANCE.md") &&
            archive.Contains("Content/Fonts/OFL-1.1.txt") &&
            std::ranges::all_of(archive.Entries(), [](const auto& entry) {
                return !entry.name.empty() && !entry.name.starts_with('/') &&
                       entry.name.find('\\') == std::string::npos &&
                       entry.name.find(":") == std::string::npos &&
                       entry.name.find("../") == std::string::npos;
            }),
        "V2 package owns both locale media sets and has no repository escape path");
    std::vector<px::progress::SaveMigrationDescriptor> migrations;
    for (const auto& value : manifest.saveMigrations) {
        migrations.push_back({
            value.id, value.fromContentVersion, value.fromSaveVersion,
            value.toContentVersion, value.toSaveVersion, value.asset});
    }
    const auto migrated = px::progress::MigrateSaveSnapshot(
        *loaded,
        {manifest.gameId, manifest.packageFingerprint, manifest.contentVersion,
         manifest.saveVersion},
        migrations, [&archive](const std::string_view asset) {
            return ReadArchiveText(archive, asset);
        });
    suite.Require(migrated.IsOk(),
                  "version-one slot migrates through the packaged migration asset");
    const auto& value = migrated.Value();
    const auto choice = value.typedVariables.find("eveningChoice");
    const auto journeyVersion =
        value.typedVariables.find("releaseJourneyVersion");
    suite.Expect(
        value.contentVersion == "journey-v2" && value.saveVersion == 2 &&
            value.packageFingerprint == manifest.packageFingerprint &&
            !value.typedVariables.contains("walkChoice") &&
            choice != value.typedVariables.end() &&
            choice->second.TryGet<std::string>() &&
            *choice->second.TryGet<std::string>() == "long-night" &&
            journeyVersion != value.typedVariables.end() &&
            journeyVersion->second.TryGet<std::int64_t>() &&
            *journeyVersion->second.TryGet<std::int64_t>() == 2,
        "migration renames the authored variable and records the V2 version");
    suite.Require(saves.Save(7, value),
                  "migrated slot is committed with the current stable key");
    const auto roundTrip = saves.Load(7);
    suite.Expect(roundTrip && roundTrip->contentVersion == "journey-v2" &&
                     roundTrip->typedVariables.contains("eveningChoice"),
                 "migrated slot reads back after the package upgrade");

    const auto protectedPath = saveRoot / "protected-wrong-key.dat";
    const std::string protectedBytes = Read(protectedPath);
    px::progress::GlobalProfile replacement;
    replacement.MarkSeen("replacement-must-not-win");
    suite.Expect(
        px::progress::LoadGlobalProfileWithFallback(
            replacement, protectedPath.string(), &key, fallbackKeys) ==
                px::progress::PersistLoadStatus::Failed &&
            !px::progress::SaveGlobalProfilePreservingExisting(
                replacement, protectedPath.string(), &key, fallbackKeys) &&
            Read(protectedPath) == protectedBytes,
        "wrong-key user data remains byte-identical after the failed V2 read");
    return true;
}

}  // namespace

int main(const int argc, char** argv) {
    px::test::Suite suite("ReleaseJourney");
    suite.Run("PackagedArtifactsOnly", [&] {
        suite.Require(argc == 3,
                      "verifier receives a mode and packaged release directory");
        const std::string_view mode = argv[1];
        const std::filesystem::path release =
            std::filesystem::absolute(std::filesystem::path(argv[2]));
        suite.Require(std::filesystem::is_regular_file(
                          release / "Package/manifest.json") &&
                          std::filesystem::is_regular_file(
                              release / "Content.pdx"),
                      "verifier consumes only packaged release artifacts");
        if (mode == "write-v1") {
            (void)WriteVersionOne(suite, release);
        } else if (mode == "verify-v2") {
            (void)VerifyVersionTwo(suite, release);
        } else {
            suite.Require(false, "verifier mode is write-v1 or verify-v2");
        }
    });
    return suite.Finish();
}
