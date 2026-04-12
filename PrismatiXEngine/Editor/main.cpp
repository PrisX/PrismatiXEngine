#include <SDL2/SDL.h>
#include <filesystem>
#include <iostream>
#include <string>

#include "Core/Engine.h"
#include "Core/EngineConfig.h"
#include "EditorApp.h"
#include "Utils/Logger.h"

// Forward declaration is not enough when we call methods, so include the full header:
#include "Core/Services/ResourceManager.h"

#pragma execution_character_set("utf-8")

namespace {
void SetWorkingDirectoryToExecutable(int argc, char* argv[]) {
    std::filesystem::path executableDir;
    char* basePath = SDL_GetBasePath();
    if (basePath) {
        executableDir = std::filesystem::path(basePath);
        SDL_free(basePath);
    } else if (argc > 0 && argv && argv[0]) {
        executableDir = std::filesystem::path(argv[0]).parent_path();
    }
    if (executableDir.empty()) return;
    std::error_code ec;
    std::filesystem::current_path(executableDir, ec);
}
}  // namespace

int main(int argc, char* argv[]) {
    SetWorkingDirectoryToExecutable(argc, argv);
    std::filesystem::create_directories("logs");
    Logger::Initialize();

    PX_LOG_INFO("Starting PrismatiX Editor...");

    // We instantiate the engine to get SDL2 contexts, audio, resource managers, etc.
    PrismatiX::App::Engine engine;

    if (!engine.Initialize("PrismatiX Editor", EngineConfig::kDefaultScreenWidth, EngineConfig::kDefaultScreenHeight)) {
        PX_LOG_CRITICAL("Engine context initialization failed.");
        return -1;
    }

    PX_LOG_INFO("Scanning asset directories...");
    engine.GetResourceManager().ScanDirectory("Data");
    engine.GetResourceManager().ScanDirectory("Engine");
    
    // Mount archives just like the game
    engine.GetResourceManager().MountArchive(std::string(EngineConfig::kArchiveEngine));
    engine.GetResourceManager().MountArchive(std::string(EngineConfig::kArchiveData));

    // Initialize Editor Application
    PrismatiX::Editor::EditorApp editorApp(engine);
    if (!editorApp.Initialize()) {
        PX_LOG_CRITICAL("EditorApp initialization failed.");
        return -1;
    }

    // Standalone Editor Loop (Bypasses Lua)
    while (engine.IsRunning()) {
        // Handle SDL Events
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            editorApp.ProcessEvent(event);
            if (event.type == SDL_QUIT) {
                engine.Quit();
            }
        }

        // Render Frame
        engine.ClearScreen();
        editorApp.NewFrame();
        editorApp.Render();
        engine.PresentScreen();
    }

    PX_LOG_INFO("Editor loop finished. Shutting down.");
    return 0;
}
