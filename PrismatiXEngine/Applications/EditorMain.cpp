#include "Editor/Application/EditorApp.h"
#include "Editor/Build/BuildService.h"
#include "Editor/Project/ProjectService.h"
#include "Engine/Support/Logger.h"

#include <SDL3/SDL_main.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <DbgHelp.h>
#endif

namespace {
#ifdef _WIN32
LONG WINAPI WriteEditorMinidump(EXCEPTION_POINTERS* exception) {
    std::error_code error;
    const auto directory = std::filesystem::current_path() / ".prismatix" / "Crashes";
    std::filesystem::create_directories(directory, error);
    const auto path = directory / ("PrismatiXEditor-" +
        std::to_string(GetCurrentProcessId()) + ".dmp");
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION information{GetCurrentThreadId(), exception, TRUE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          MiniDumpWithThreadInfo, &information, nullptr, nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif
std::filesystem::path PlayerExecutableName() {
#ifdef _WIN32
    return "PrismatiXPlayer.exe";
#else
    return "PrismatiXPlayer";
#endif
}

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
        std::filesystem::absolute(argv0, ec).parent_path() / PlayerExecutableName();
    opt.title = m.name;
    opt.startRoute = m.startRoute;
    opt.routes = m.routes;
    opt.startScript = m.startScript;
    opt.key = m.encryptKey;
    opt.encrypt = m.encrypt;
    opt.gameWidth = m.gameWidth;
    opt.gameHeight = m.gameHeight;
    const auto profilePath = project.Context().root /
                             ".prismatix/ExportProfiles/windows-release.pxexport";
    if (std::ifstream stream(profilePath, std::ios::binary); stream) {
        std::ostringstream text; text << stream.rdbuf();
        auto profile = px::editor::ParseExportProfile(text.str(), profilePath.generic_string());
        if (!profile) return 1;
        opt.profile = profile.TakeValue();
    }
    return builder.Build(opt) ? 0 : 1;
}

int RunCreateProject(const std::filesystem::path& root, const std::string& name) {
    auto log = [](const std::string& text) { PX_LOG_INFO("[create] {}", text); };
    px::editor::ProjectService project(log);
    const auto bundledFont = std::filesystem::current_path() /
                             "Resources/Fonts/NotoSansTC-Bold.ttf";
    return project.Create(root, name, bundledFont) ? 0 : 1;
}
}

int main(int argc, char* argv[]) {
    Logger::Initialize("PrismatiXEditor");
#ifdef _WIN32
    SetUnhandledExceptionFilter(&WriteEditorMinidump);
#endif

    if (argc > 1 && std::string(argv[1]) == "--build") {
        const int result = RunHeadlessBuild(argv[0]);
        Logger::Shutdown();
        return result;
    }
    if (argc > 2 && std::string(argv[1]) == "--create-project") {
        const std::string name = argc > 3 ? argv[3] : "PrismatiX Project";
        const int result = RunCreateProject(argv[2], name);
        Logger::Shutdown();
        return result;
    }

    px::editor::EditorApp app;
    if (!app.Init()) {
        Logger::Shutdown();
        return 1;
    }
    if (argc > 1 && std::string(argv[1]) == "--open") {
        app.OpenWorkspace();
    }
    app.Run();
    app.Shutdown();
    Logger::Shutdown();
    return 0;
}
