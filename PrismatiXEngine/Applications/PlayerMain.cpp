#include "Applications/Player/PlayerApp.h"

#include "Engine/Support/Logger.h"
#include "Engine/Progression/Persist.h"
#include "Engine/Progression/UserDataLocation.h"
#include "Engine/Package/PackageManifest.h"
#include "Engine/Support/Utf8Path.h"

#include <SDL3/SDL_main.h>
#include <SDL3/SDL_filesystem.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <fstream>
#include <iostream>
#include <optional>

namespace {
std::optional<std::string> PackagedGameId(const std::filesystem::path& root) {
    const auto manifest = root / "Package/manifest.json";
    std::error_code error;
    const auto size = std::filesystem::file_size(manifest, error);
    if (error || size == 0 || size > 4u * 1024u * 1024u) return std::nullopt;
    std::ifstream input(manifest, std::ios::binary);
    if (!input) return std::nullopt;
    std::string text(static_cast<std::size_t>(size), '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!input) return std::nullopt;
    const auto parsed = px::sdk::detail::ParsePackageManifest(text);
    if (!parsed.Valid()) return std::nullopt;
    return parsed.manifest.gameId;
}

// Keep this translation unit coupled to PlayerApp's complete class layout;
// localized MSVC /showIncludes output cannot currently drive Ninja header
// dependencies reliably on every host. Recompile this TU whenever PlayerApp's
// state layout changes.
int InspectSave(int argc, char* argv[]) {
    // Diagnostics are collected below on stderr; never initialize a file logger
    // or send the default spdlog console sink into the machine JSON channel.
    spdlog::set_level(spdlog::level::off);
    try {
        if (argc < 3) throw std::invalid_argument("--inspect-save requires a path");
        std::string packagePath, secret;
        bool json = false, secretProvided = false;
        for (int index = 3; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--json") json = true;
            else if ((argument == "--package" || argument == "--secret") && index + 1 < argc) {
                (argument == "--package" ? packagePath : secret) = argv[++index];
                if (argument == "--secret") secretProvided = true;
            } else throw std::invalid_argument("Unknown or incomplete inspect-save option: " + argument);
        }
        std::optional<px::crypto::Key> key;
        std::vector<px::crypto::Key> fallback;
        if (!packagePath.empty()) {
            const std::filesystem::path path(std::u8string(packagePath.begin(), packagePath.end()));
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error || size > 4u * 1024u * 1024u)
                throw std::invalid_argument("Package manifest is unreadable or exceeds 4 MiB");
            std::ifstream input(path, std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(input), {}};
            const auto package = px::sdk::detail::ParsePackageManifest(text);
            if (!package.Valid()) throw std::invalid_argument("Package manifest is invalid");
            if (package.manifest.persistenceKeyId == px::progress::kGameDataPersistenceKeyId) {
                key = px::progress::DeriveGameDataPersistenceKey(package.manifest.persistenceDataId);
                for (const auto& reader : package.manifest.legacyPersistenceReaders)
                    fallback.push_back(px::progress::DeriveLegacyPersistenceKey(reader.secret));
            } else {
                key = px::progress::DeriveLegacyPersistenceKey(package.manifest.archiveKey.empty()
                    ? package.manifest.packageFingerprint : package.manifest.archiveKey);
            }
        } else if (secretProvided) key = px::progress::DeriveLegacyPersistenceKey(secret);
        const std::string saveArgument = argv[2];
        const std::filesystem::path savePath(std::u8string(saveArgument.begin(), saveArgument.end()));
        auto loaded = px::progress::LoadJsonPathWithFallback(savePath, key ? &*key : nullptr, fallback);
        if (!loaded.Loaded()) throw std::invalid_argument("Save cannot be read with the selected persistence key");
        const auto snapshot = px::progress::ParseSaveEnvelope(loaded.value->dump(), argv[2]);
        if (!snapshot) throw std::invalid_argument("Save envelope validation failed");
        const px::progress::Json metadata{
            {"format", "PrismatiXSave"}, {"schemaRevision", 4},
            {"engineVersion", snapshot->engineVersion}, {"gameId", snapshot->gameId},
            {"packageFingerprint", snapshot->packageFingerprint},
            {"contentVersion", snapshot->contentVersion}, {"saveVersion", snapshot->saveVersion},
            {"anchor", loaded.value->at("anchor")}, {"timestamp", snapshot->timestamp},
            {"playtimeMs", snapshot->playtimeMs}, {"chapter", snapshot->chapter}};
        std::cout << (json ? loaded.value->dump() : metadata.dump()) << '\n';
        return 0;
    } catch (const std::exception& error) {
        for (const auto& diagnostic : px::diag::Global().Snapshot())
            std::cerr << px::diag::Describe(diagnostic) << '\n';
        std::cerr << "PXPLAYER-INSPECT-SAVE: " << error.what() << '\n';
        return 1;
    }
}
}

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string_view(argv[1]) == "--inspect-save") return InspectSave(argc, argv);
    // The package owns only immutable game content. A shortcut's working
    // directory never chooses persistence, which lives in per-user storage.
    // Keep the existing cwd-based developer launch when no adjacent package
    // exists; script arguments retain their original virtual asset paths.
    std::filesystem::path logDirectory = "logs";
    if (const char* base = SDL_GetBasePath()) {
        const std::filesystem::path packageRoot(
            std::u8string(base, base + std::char_traits<char>::length(base)));
        std::error_code error;
        if (std::filesystem::is_regular_file(packageRoot / "Package/manifest.json", error)) {
            std::filesystem::current_path(packageRoot, error);
            if (error) {
                std::fprintf(stderr, "PXPLAYER5003: Cannot enter packaged game directory: %s\n",
                             error.message().c_str());
                return 1;
            }
            const auto userData = px::progress::ResolvePackagedUserDataDirectory(
                PackagedGameId(packageRoot).value_or("invalid-package"));
            if (!userData) {
                for (const auto& diagnostic : userData.Diagnostics())
                    std::fprintf(stderr, "%s: %s (%s)\n",
                                 diagnostic.code.c_str(),
                                 diagnostic.message.c_str(),
                                 diagnostic.details.c_str());
                return 1;
            }
            logDirectory = userData.Value() / "logs";
        }
    }
    if (!Logger::Initialize("PrismatiXPlayer", logDirectory)) return 1;
    px::player::PlayerApp app;
    const int result = app.Run(argc, argv);
    Logger::Shutdown();
    return result;
}
