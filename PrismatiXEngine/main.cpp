#include <SDL2/SDL.h>

#include <filesystem>
#include <iostream>
#include <string>

#include "Core/Engine.h"
#include "Core/EngineConfig.h"
#include "Core/Services/ResourceManager.h"
#include "Utils/Logger.h"

#pragma execution_character_set("utf-8")

namespace {
void SetWorkingDirectoryToExecutable(int argc, char* argv[]) {
    std::filesystem::path executableDir;

    char* basePath = SDL_GetBasePath();
    if (basePath) {
        executableDir = std::filesystem::path(basePath);
        SDL_free(basePath);
    }
    else if (argc > 0 && argv && argv[0]) {
        executableDir = std::filesystem::path(argv[0]).parent_path();
    }

    if (executableDir.empty()) return;

    std::error_code ec;
    std::filesystem::current_path(executableDir, ec);
}

bool RunLuaEntrypoint(PrismatiX::App::Engine& engine, const std::string& scriptPath) {
    PX_LOG_INFO("Loading entrypoint: {}", scriptPath);
    std::string script = engine.GetResourceManager().LoadText(scriptPath);
    if (script.empty()) {
        PX_LOG_ERROR("Lua script not found or empty: {}", scriptPath);
        return false;
    }

    auto result = engine.GetLuaState().safe_script(script, sol::script_pass_on_error);
    if (!result.valid()) {
        sol::error err = result;
        PX_LOG_CRITICAL("Failed to execute entrypoint: {}", err.what());
        return false;
    }

    sol::protected_function entry = engine.GetLuaState()["Entrypoint"];
    if (!entry.valid()) {
        PX_LOG_ERROR("'Entrypoint' function not found in {}", scriptPath);
        return false;
    }

    PX_LOG_INFO("Calling Entrypoint()...");
    auto runResult = entry();
    if (!runResult.valid()) {
        sol::error err = runResult;
        PX_LOG_CRITICAL("Lua Runtime Error: {}", err.what());
        return false;
    }
    return true;
}
}  // namespace

int main(int argc, char* argv[]) {
    SetWorkingDirectoryToExecutable(argc, argv);
    std::filesystem::create_directories("logs");
    Logger::Initialize();

    PrismatiX::App::Engine engine;

    if (!engine.Initialize(std::string(EngineConfig::kGameTitle), EngineConfig::kDefaultScreenWidth, EngineConfig::kDefaultScreenHeight)) {
        PX_LOG_CRITICAL("Engine initialization failed.");
        return -1;
    }

    PX_LOG_INFO("Scanning asset directories...");
    engine.GetResourceManager().ScanDirectory("Data");
    engine.GetResourceManager().ScanDirectory("Engine");
    engine.GetResourceManager().ScanDirectory("Scripts");

    PX_LOG_INFO("Mounting archives...");
    engine.GetResourceManager().MountArchive(std::string(EngineConfig::kArchiveEngine));
    engine.GetResourceManager().MountArchive(std::string(EngineConfig::kArchiveData));

    if (!RunLuaEntrypoint(engine, "Scripts/entrypoint.lua")) {
        PX_LOG_CRITICAL("Entrypoint script execution failed.");
    }

    PX_LOG_INFO("Main loop finished. Shutting down.");
    return 0;
}
