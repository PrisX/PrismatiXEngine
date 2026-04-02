#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "Managers/ArchiveManager.h"
#include "Core/PrismatiXEngine.h"
#include "Core/EngineConfig.h"

#pragma execution_character_set("utf-8")

namespace {
bool ExecuteLuaFile(PrismatiXEngine& engine, const std::string& scriptPath) {
    std::string scriptContent = engine.GetArchiveManager().LoadTextFromArchiveOrDisk(scriptPath);
    if (scriptContent.empty()) {
        std::cerr << "Lua script not found: " << scriptPath << std::endl;
        return false;
    }

    sol::protected_function_result loadResult = engine.GetLuaState().safe_script(scriptContent, &sol::script_pass_on_error);
    if (!loadResult.valid()) {
        sol::error err = loadResult;
        std::cerr << "Failed to load Lua script (" << scriptPath << "): " << err.what() << std::endl;
        return false;
    }
    return true;
}

bool RunLuaEntrypoint(PrismatiXEngine& engine, const std::string& entryScriptPath) {
    if (!ExecuteLuaFile(engine, entryScriptPath)) {
        return false;
    }

    sol::protected_function entrypoint = engine.GetLuaState()["Entrypoint"];
    if (!entrypoint.valid()) {
        std::cerr << "Entrypoint() not found in script: " << entryScriptPath << std::endl;
        return false;
    }

    sol::protected_function_result runResult = entrypoint();
    if (!runResult.valid()) {
        sol::error err = runResult;
        std::cerr << "Entrypoint() runtime error (" << entryScriptPath << "): " << err.what() << std::endl;
        return false;
    }
    return true;
}
}  // namespace

int main(int argc, char* argv[]) {
    PrismatiXEngine engine;

    if (!engine.GetArchiveManager().MountArchive(EngineConfig::kArchiveEngine)) {
        std::cerr << "Failed to mount engine assets." << std::endl;
        return -1;
    }
    engine.GetArchiveManager().MountArchive(EngineConfig::kArchiveData);

    if (!engine.Initialize(EngineConfig::kGameTitle, EngineConfig::kDefaultScreenWidth, EngineConfig::kDefaultScreenHeight)) {
        return -1;
    }

    const std::string entryScriptPath = "Scripts/entrypoint.lua";
    bool ok = RunLuaEntrypoint(engine, entryScriptPath);

    return ok ? 0 : -1;
}