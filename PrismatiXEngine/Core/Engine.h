#pragma once

#include <SDL2/SDL.h>

#include <lua.hpp>
#include <memory>
#include <sol/sol.hpp>
#include <string>

#include "EngineConfig.h"
#include "Services/GameState.h"
#include "Services/ResourceManager.h"
#include "Services/VNScriptParser.h"
#include "Systems/AudioSystem.h"
#include "Systems/RenderSystem.h"
#include "UI/UIManager.h"
#include "Utils/Logger.h"

class Engine {
public:
    Engine();
    ~Engine();

    bool Initialize(const std::string& title, int width, int height);
    void Clean();

    bool IsRunning() const { return isRunning; }
    SDL_Renderer* GetRenderer() const { return renderer; }
    SDL_Window* GetWindow() const { return window; }

    // Input
    bool GetLeftClick() const { return leftClick; }
    bool GetRightClick() const { return rightClick; }
    int GetMouseWheelY() const { return mouseWheelY; }
    int GetMouseX() const { return mouseX; }
    int GetMouseY() const { return mouseY; }

    // Viewport
    void SetCameraOffset(int x, int y);
    void ResetCameraOffset();
    void HandleEvents();
    void ClearScreen();
    void PresentScreen();

    // Lua
    void BindEngineToLua();
    sol::state& GetLuaState() { return lua; }

    // Services Accessors
    ResourceManager& GetResourceManager() { return *resourceManager; }
    VNScriptParser& GetScriptingEngine() { return *scriptingEngine; }
    GameState& GetGameState() { return *gameState; }
    AudioSystem& GetAudioSystem() { return *audioSystem; }
    RenderSystem& GetRenderSystem() { return *renderSystem; }
    UIManager& GetUIManager() { return *uiManager; }

private:
    int lastWinW = EngineConfig::kDefaultScreenWidth;
    int lastWinH = EngineConfig::kDefaultScreenHeight;
    bool leftClick;
    bool rightClick;
    int mouseWheelY;
    bool isRunning;
    int mouseX = 0;
    int mouseY = 0;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    sol::state lua;

    // Core Services
    std::unique_ptr<ResourceManager> resourceManager;
    std::unique_ptr<VNScriptParser> scriptingEngine;
    std::unique_ptr<GameState> gameState;

    // Systems
    std::unique_ptr<AudioSystem> audioSystem;
    std::unique_ptr<RenderSystem> renderSystem;
    std::unique_ptr<UIManager> uiManager;
};
