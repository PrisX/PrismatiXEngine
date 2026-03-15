#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "Managers/ArchiveManager.h"
#include "Managers/AudioManager.h"
#include "Managers/TextManager.h"
#include "PrismatiXEngine.h"

#pragma execution_character_set("utf-8")

namespace {
std::string LoadTextFromArchiveOrDisk(const std::string& path) {
    std::vector<char> archiveBuffer = ArchiveManager::ExtractFile(path);
    if (!archiveBuffer.empty()) {
        return std::string(archiveBuffer.begin(), archiveBuffer.end());
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return "";
    }

    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool ExecuteLuaFile(PrismatiXEngine& engine, const std::string& scriptPath) {
    std::string scriptContent = LoadTextFromArchiveOrDisk(scriptPath);
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
    if (!ArchiveManager::MountArchive("Engine.pdx")) {
        std::cerr << "Failed to mount engine assets." << std::endl;
        return -1;
    }

    ArchiveManager::MountArchive("Data.pdx");

    const std::string gameTitle = "PrismatiX Engine";
    const int winW = 1280;
    const int winH = 720;

    PrismatiXEngine engine;
    if (!engine.Initialize(gameTitle, winW, winH)) {
        return -1;
    }

    const std::string entryScriptPath = "Scripts/entrypoint.lua";

    bool ok = RunLuaEntrypoint(engine, entryScriptPath);

    TextManager::Clean();
    AudioManager::CleanCache();

    return ok ? 0 : -1;
}