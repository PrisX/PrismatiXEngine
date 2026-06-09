#include "Editor/Build/BuildService.h"

#include "Editor/Assets/AssetMeta.h"
#include "Engine/IO/Archive.h"
#include "Engine/IO/Crypto.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <optional>

namespace px::editor {

namespace {
using Bytes = px::io::Bytes;

std::string ToRuntimePath(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::relative(path, root, ec).generic_string();
}

[[nodiscard]] std::optional<Bytes> ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::nullopt;
    }

    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);

    Bytes data(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), size)) {
        return std::nullopt;
    }
    return data;
}

bool CopyFile(const std::filesystem::path& from, const std::filesystem::path& to,
              std::error_code& ec) {
    std::filesystem::create_directories(to.parent_path(), ec);
    if (ec) {
        return false;
    }
    return std::filesystem::copy_file(from, to,
                                      std::filesystem::copy_options::overwrite_existing, ec);
}

bool ShouldCopyRuntimeLibrary(const std::filesystem::path& path) {
    const std::string ext = path.extension().string();
#if defined(_WIN32)
    return ext == ".dll";
#elif defined(__APPLE__)
    return ext == ".dylib";
#else
    return ext == ".so";
#endif
}
}

bool BuildService::Build(const BuildOptions& options) const {
    if (options.projectRoot.empty() || !std::filesystem::exists(options.projectRoot)) {
        Log("Build failed: project root does not exist.");
        return false;
    }

    const std::filesystem::path dataRoot = options.projectRoot / "Data";
    if (!std::filesystem::exists(dataRoot)) {
        Log("Build failed: project has no Data/ folder.");
        return false;
    }

    if (options.playerExe.empty() || !std::filesystem::exists(options.playerExe)) {
        Log("Build failed: player executable not found at " + options.playerExe.string());
        return false;
    }

    std::error_code ec;
    std::filesystem::remove_all(options.outputDir, ec);
    ec.clear();
    std::filesystem::create_directories(options.outputDir, ec);
    if (ec) {
        Log("Build failed: could not create output directory " + options.outputDir.string());
        return false;
    }

    const std::filesystem::path archivePath = options.outputDir / "Data.pdx";
    px::io::ArchiveWriter archive;
    archive.SetCompression(true);
    if (options.encrypt) {
        archive.SetKey(px::crypto::DeriveKey(options.key));
    }

    std::size_t entryCount = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(dataRoot, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            Log("Build warning: stopped asset scan after filesystem error: " + ec.message());
            break;
        }
        if (!it->is_regular_file(ec)) {
            continue;
        }

        const std::filesystem::path file = it->path();
        if (file.extension() == ".meta") {
            continue;
        }
        if (!LoadAssetMeta(file).includeInBuild) {
            Log("Build: skipped " + ToRuntimePath(options.projectRoot, file));
            continue;
        }

        auto bytes = ReadFile(file);
        if (!bytes) {
            Log("Build failed: could not read " + file.string());
            return false;
        }

        archive.Add(ToRuntimePath(options.projectRoot, file), *bytes);
        ++entryCount;
    }

    if (!archive.Write(archivePath.string())) {
        Log("Build failed: could not write archive.");
        return false;
    }

    const std::filesystem::path packagedPlayer = options.outputDir / options.playerExe.filename();
    ec.clear();
    if (!CopyFile(options.playerExe, packagedPlayer, ec)) {
        Log("Build failed: could not copy player executable: " + ec.message());
        return false;
    }
    std::filesystem::permissions(packagedPlayer,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add, ec);

    const std::filesystem::path playerDir = options.playerExe.parent_path();
    for (auto it = std::filesystem::directory_iterator(playerDir, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || !ShouldCopyRuntimeLibrary(it->path())) {
            continue;
        }
        std::error_code copyEc;
        CopyFile(it->path(), options.outputDir / it->path().filename(), copyEc);
    }

    nlohmann::json manifest;
    manifest["title"] = options.title;
    manifest["archive"] = "Data.pdx";
    manifest["encrypt"] = options.encrypt;
    manifest["key"] = options.encrypt ? options.key : "";
    manifest["gameWidth"] = options.gameWidth;
    manifest["gameHeight"] = options.gameHeight;
    manifest["startUi"] = options.startUI;
    manifest["startScript"] = options.startScript;

    std::ofstream out(options.outputDir / "game.prismatix", std::ios::binary | std::ios::trunc);
    if (!out) {
        Log("Build failed: could not write game.prismatix.");
        return false;
    }
    out << manifest.dump(2);

    Log("Build complete: " + archivePath.string() + " (" + std::to_string(entryCount) +
        " assets, encrypted=" + std::string(options.encrypt ? "true" : "false") + ")");
    return true;
}

}
