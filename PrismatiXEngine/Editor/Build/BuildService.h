#pragma once

#include "Editor/Project/ProjectTypes.h"
#include "Editor/Build/ExportProfile.h"

#include <filesystem>
#include <string>
#include <utility>

namespace px::editor {

struct BuildOptions {
    std::filesystem::path projectRoot;
    std::filesystem::path outputDir;
    std::filesystem::path playerExe;
    std::string title = "PrismatiX Player";
    std::string startRoute = "title";
    std::vector<ProjectManifest::Route> routes;
    std::string startScript = "Content/Scenario/start.pxscenario";
    std::string key = "prismatix";
    bool encrypt = true;
    int gameWidth = 1280;
    int gameHeight = 720;
    ExportProfile profile;
};

class BuildService {
public:
    explicit BuildService(LogSink log = {}) : m_log(std::move(log)) {}

    [[nodiscard]] bool Build(const BuildOptions& options) const;

private:
    void Log(const std::string& msg) const {
        if (m_log) m_log(msg);
    }

    LogSink m_log;
};

}
