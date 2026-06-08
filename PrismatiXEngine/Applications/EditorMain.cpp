#include "Editor/Application/EditorApp.h"
#include "Editor/Build/BuildService.h"
#include "Editor/Project/ProjectService.h"
#include "Engine/Support/Logger.h"

#include <SDL3/SDL_main.h>

#include <filesystem>
#include <string>

namespace {
int RunHeadlessBuild(const char* argv0) {
    auto log = [](const std::string& s) { PX_LOG_INFO("[build] {}", s); };
    px::editor::ProjectService project(log);
    if (!project.Open(std::filesystem::current_path())) {
        return 1;
    }
    const px::editor::ProjectManifest& m = project.Context().manifest;

    px::editor::BuildService builder(log);
    px::editor::BuildOptions opt;
    opt.projectRoot = project.Context().root;
    opt.outputDir = project.Context().ExportRoot();
    std::error_code ec;
    opt.playerExe =
        std::filesystem::absolute(argv0, ec).parent_path() / "PrismatiXPlayer.exe";
    opt.title = m.name;
    opt.startUI = m.startUI;
    opt.startScript = m.startScript;
    opt.key = m.encryptKey;
    opt.encrypt = m.encrypt;
    opt.gameWidth = m.gameWidth;
    opt.gameHeight = m.gameHeight;
    return builder.Build(opt) ? 0 : 1;
}
}

int main(int argc, char* argv[]) {
    Logger::Initialize();

    if (argc > 1 && std::string(argv[1]) == "--build") {
        return RunHeadlessBuild(argv[0]);
    }

    px::editor::EditorApp app;
    if (!app.Init()) {
        return 1;
    }
    if (argc > 1 && std::string(argv[1]) == "--open") {
        app.OpenWorkspace();
    }
    app.Run();
    app.Shutdown();
    return 0;
}
