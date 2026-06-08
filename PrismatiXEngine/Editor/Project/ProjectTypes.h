#pragma once

#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace px::editor {

using Json = nlohmann::json;
using LogSink = std::function<void(const std::string&)>;

struct ProjectManifest {
    std::string name = "PrismatiX Project";
    int version = 2;
    int gameWidth = 1280;
    int gameHeight = 720;
    std::string startUI = "Data/UI/title.pxui";
    std::string startScript = "test_scene.pds";
    std::string theme = "PrismatiX Dark";
    std::vector<std::string> assetRoots{ "Data" };
    bool encrypt = true;
    std::string encryptKey = "prismatix";
    bool singleFile = false;
};

struct ProjectContext {
    std::filesystem::path root;
    ProjectManifest manifest;

    [[nodiscard]] bool IsOpen() const { return !root.empty(); }
    [[nodiscard]] std::filesystem::path DataRoot() const { return root / "Data"; }
    [[nodiscard]] std::filesystem::path ExportRoot() const { return root / "Export"; }
    [[nodiscard]] std::filesystem::path ManifestPath() const { return root / "project.prismatix.json"; }
};

}
