#include "Engine/Progression/UserDataLocation.h"

#include "Engine/IO/Crypto.h"
#include "Engine/IO/AtomicFile.h"
#include "Engine/Support/Utf8Path.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <system_error>

namespace px::progress {
namespace {

diag::Diagnostic DataError(const std::filesystem::path& path,
                           std::string message, std::string details = {}) {
    diag::Diagnostic diagnostic{
        .severity = diag::Severity::Fatal,
        .code = "PXPLAYER5025",
        .category = "Player.Persistence",
        .message = std::move(message),
        .details = std::move(details)};
    diagnostic.source.path = PathToUtf8(path);
    return diagnostic;
}

bool IsLegacyBackupSuffix(const std::string_view suffix) {
    if (suffix == ".legacy.bak") return true;
    if (!suffix.starts_with(".legacy.") || !suffix.ends_with(".bak"))
        return false;
    const auto number = suffix.substr(8, suffix.size() - 12);
    return !number.empty() && number.size() <= 2 &&
           std::ranges::all_of(number, [](const unsigned char character) {
               return std::isdigit(character) != 0;
           });
}

bool IsLegacySaveName(const std::string_view name) {
    std::string_view base = name;
    if (const auto backup = name.find(".legacy."); backup != name.npos) {
        if (!IsLegacyBackupSuffix(name.substr(backup))) return false;
        base = name.substr(0, backup);
    }
    if (base == "profile.dat" || base == "config.dat" ||
        base == "autosave.pxsav") return true;
    if (!base.starts_with("save_") || !base.ends_with(".pxsav")) return false;
    const auto number = base.substr(5, base.size() - 11);
    return !number.empty() && number.size() <= 10 &&
           std::ranges::all_of(number, [](const unsigned char character) {
               return std::isdigit(character) != 0;
           });
}

bool ValidOptionalPackId(const std::string_view value) {
    return !value.empty() && value.size() <= 64 &&
           std::ranges::all_of(value, [](const unsigned char c) {
               return (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-';
           });
}

}  // namespace

Result<std::filesystem::path> ResolvePackagedUserDataDirectory(
    const std::string_view gameId) {
    if (gameId.empty())
        return Result<std::filesystem::path>::Failure(DataError(
            {}, "Cannot resolve user data without a game ID"));
    // Only letters, numbers and spaces enter SDL_GetPrefPath's app name.
    // Hashing also avoids collisions between identifiers containing punctuation.
    const std::string appName = "Game " + crypto::Sha256Hex(gameId);
    std::filesystem::path directory;
#if defined(PRISMATIX_WEB_PLAYER)
    // SDL captures its environment before callMain, and its default Web
    // preference path is MEMFS. Always place shipped Web saves on IDBFS.
    directory = std::filesystem::path("/persist") / PathFromUtf8(appName);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
        return Result<std::filesystem::path>::Failure(DataError(
            directory, "Could not create browser persistence directory", error.message()));
#else
    if (const char* overrideRoot = SDL_getenv("PRISMATIX_PLAYER_DATA_ROOT");
        overrideRoot && *overrideRoot) {
        const auto parent = PathFromUtf8(overrideRoot);
        if (!parent.is_absolute())
            return Result<std::filesystem::path>::Failure(DataError(
                parent, "PRISMATIX_PLAYER_DATA_ROOT must be an absolute path"));
        directory = parent / PathFromUtf8(appName);
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error)
            return Result<std::filesystem::path>::Failure(DataError(
                directory, "Could not create the packaged game's user data directory",
                error.message()));
    } else {
        char* prefPath = SDL_GetPrefPath("PrismatiXEngine", appName.c_str());
        if (!prefPath)
            return Result<std::filesystem::path>::Failure(DataError(
                {}, "Could not resolve the packaged game's user data directory",
                SDL_GetError()));
        directory = PathFromUtf8(prefPath);
        SDL_free(prefPath);
    }
#endif
    return Result<std::filesystem::path>::Success(directory.lexically_normal());
}

Result<std::string> ReadEnabledOptionalPackId(
    const std::filesystem::path& userDataDirectory) {
    const auto path = userDataDirectory / "optional-pack.txt";
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && status.type() == std::filesystem::file_type::not_found))
        return Result<std::string>::Success({});
    if (error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status))
        return Result<std::string>::Failure(DataError(
            path, "Optional pack preference is not a regular file"));
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > 65)
        return Result<std::string>::Failure(DataError(
            path, "Optional pack preference is oversized"));
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return Result<std::string>::Failure(DataError(
            path, "Optional pack preference cannot be read"));
    std::string value(static_cast<std::size_t>(size), '\0');
    if (!input.read(value.data(), static_cast<std::streamsize>(size)))
        return Result<std::string>::Failure(DataError(
            path, "Optional pack preference cannot be read"));
    if (!value.empty() && value.back() == '\n') value.pop_back();
    if (!value.empty() && !ValidOptionalPackId(value))
        return Result<std::string>::Failure(DataError(
            path, "Optional pack preference contains an invalid ID"));
    return Result<std::string>::Success(std::move(value));
}

Result<bool> WriteEnabledOptionalPackId(
    const std::filesystem::path& userDataDirectory,
    const std::string_view optionalPackId) {
    const auto path = userDataDirectory / "optional-pack.txt";
    if (!optionalPackId.empty() && !ValidOptionalPackId(optionalPackId))
        return Result<bool>::Failure(DataError(
            path, "Optional pack ID is invalid"));
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory)
        return Result<bool>::Failure(DataError(
            path, "Optional pack preference cannot be inspected"));
    if (!error && status.type() != std::filesystem::file_type::not_found &&
        (!std::filesystem::is_regular_file(status) ||
         std::filesystem::is_symlink(status)))
        return Result<bool>::Failure(DataError(
            path, "Optional pack preference is not a regular file"));
    const std::string text = std::string(optionalPackId) + '\n';
    if (const auto written = io::AtomicFile::WriteText(path, text); !written)
        return Result<bool>::Failure(DataError(
            path, "Optional pack preference could not be saved",
            written.Diagnostics().empty() ? std::string{} :
                written.Diagnostics().front().message));
    return Result<bool>::Success(true);
}

Result<std::size_t> ImportLegacyPortableSaves(
    const std::filesystem::path& packageRoot,
    const std::filesystem::path& userDataDirectory) {
    const auto marker = userDataDirectory / ".portable-save-imported-v1";
    std::error_code error;
    const auto markerStatus = std::filesystem::symlink_status(marker, error);
    if (error && error != std::errc::no_such_file_or_directory)
        return Result<std::size_t>::Failure(DataError(
            marker, "Could not inspect the portable-save import marker",
            error.message()));
    if (std::filesystem::exists(markerStatus)) {
        if (!std::filesystem::is_regular_file(markerStatus))
            return Result<std::size_t>::Failure(DataError(
                marker, "Portable-save import marker is not a regular file"));
        return Result<std::size_t>::Success(0);
    }

    const auto source = packageRoot / "Save";
    const auto destination = userDataDirectory / "Save";
    const auto sourceStatus = std::filesystem::symlink_status(source, error);
    if (error && error != std::errc::no_such_file_or_directory)
        return Result<std::size_t>::Failure(DataError(
            source, "Could not inspect the legacy portable Save directory",
            error.message()));
    if (std::filesystem::exists(sourceStatus) &&
        !std::filesystem::is_directory(sourceStatus))
        return Result<std::size_t>::Failure(DataError(
            source, "Legacy portable Save path is not a real directory"));

    std::filesystem::create_directories(destination, error);
    if (error)
        return Result<std::size_t>::Failure(DataError(
            destination, "Could not create the packaged game's Save directory",
            error.message()));

    std::size_t imported = 0;
    if (std::filesystem::exists(sourceStatus)) {
        std::filesystem::directory_iterator files(source, error);
        if (error)
            return Result<std::size_t>::Failure(DataError(
                source, "Could not enumerate legacy portable saves",
                error.message()));
        std::size_t seen = 0;
        for (; files != std::filesystem::directory_iterator{};
             files.increment(error)) {
            if (error)
                return Result<std::size_t>::Failure(DataError(
                    source, "Could not enumerate legacy portable saves",
                    error.message()));
            if (++seen > 2048)
                return Result<std::size_t>::Failure(DataError(
                    source, "Legacy portable Save directory has too many entries"));
            const auto filename = PathToUtf8(files->path().filename());
            if (!IsLegacySaveName(filename)) continue;
            const auto fileStatus = files->symlink_status(error);
            if (error || !std::filesystem::is_regular_file(fileStatus))
                return Result<std::size_t>::Failure(DataError(
                    files->path(), "Legacy save is not a regular file",
                    error.message()));
            const auto size = files->file_size(error);
            if (error || size > 64ull * 1024ull * 1024ull)
                return Result<std::size_t>::Failure(DataError(
                    files->path(), "Legacy save is unreadable or exceeds 64 MiB",
                    error.message()));
            const auto target = destination / files->path().filename();
            const auto targetStatus = std::filesystem::symlink_status(target, error);
            if (error && error != std::errc::no_such_file_or_directory)
                return Result<std::size_t>::Failure(DataError(
                    target, "Could not inspect existing user save",
                    error.message()));
            if (std::filesystem::exists(targetStatus)) {
                if (!std::filesystem::is_regular_file(targetStatus))
                    return Result<std::size_t>::Failure(DataError(
                        target, "Existing user save is not a regular file"));
                continue;  // New-location data always wins; never overwrite it.
            }
            std::filesystem::copy_file(files->path(), target,
                                       std::filesystem::copy_options::none,
                                       error);
            if (error)
                return Result<std::size_t>::Failure(DataError(
                    target, "Could not import a legacy portable save",
                    error.message()));
            ++imported;
        }
        if (error)
            return Result<std::size_t>::Failure(DataError(
                source, "Could not finish enumerating legacy portable saves",
                error.message()));
    }

    std::ofstream completed(marker, std::ios::binary | std::ios::trunc);
    completed << "PrismatiX portable save import v1\n";
    if (!completed.good())
        return Result<std::size_t>::Failure(DataError(
            marker, "Could not record completion of portable-save import"));
    return Result<std::size_t>::Success(imported);
}

}  // namespace px::progress
