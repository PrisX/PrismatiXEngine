#pragma once

#include "Engine/UI/Startup/SplashTypes.h"

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
    int version = 4;
    int gameWidth = 1280;
    int gameHeight = 720;
    struct Route { std::string id; std::string scene; bool modal=false; std::string cache="recreate"; };
    std::string startRoute = "title";
    std::vector<Route> routes{{"title","Content/UI/Title.pxscene",false,"keep-alive"},{"hud","Content/UI/HUD.pxscene",false,"keep-alive"},{"backlog","Content/UI/Backlog.pxscene",true,"recreate"},{"save","Content/UI/SaveLoad.pxscene",true,"recreate"},{"load","Content/UI/SaveLoad.pxscene",true,"recreate"},{"gallery","Content/UI/Gallery.pxscene",true,"recreate"},{"settings","Content/UI/Settings.pxscene",true,"keep-alive"},{"video","Content/UI/VideoOverlay.pxscene",true,"recreate"},{"confirm","Content/UI/ConfirmDialog.pxscene",true,"recreate"}};
    std::string startScript = "Content/Scenario/start.pxscenario";
    std::string theme = "PrismatiX Dark";
    std::string uiThemePath = "Content/UI/PrismatiX.pxtheme";
    std::vector<ui::startup::SplashScreenEntry> splashes;
    std::vector<std::string> assetRoots{ "Content" };
    bool encrypt = true;
    std::string encryptKey = "prismatix";
    bool singleFile = false;
};

struct ProjectContext {
    std::filesystem::path root;
    ProjectManifest manifest;

    [[nodiscard]] bool IsOpen() const { return !root.empty(); }
    [[nodiscard]] std::filesystem::path DataRoot() const { return root / "Content"; }
    [[nodiscard]] std::filesystem::path ExportRoot() const { return root / "Export"; }
    [[nodiscard]] std::filesystem::path ManifestPath() const { return root / "project.pxproject"; }
    [[nodiscard]] std::string StartScenePath() const { for(const auto& route:manifest.routes)if(route.id==manifest.startRoute)return route.scene;return {}; }
};

}
