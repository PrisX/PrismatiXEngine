#pragma once

#include <SDL2/SDL.h>

#include <lua.hpp>
#include <memory>
#include <sol/sol.hpp>
#include <string>

#include "EngineConfig.h"
#include "ServiceContainer.h"
#include "Services/GameState.h"
#include "Services/ResourceManager.h"
#include "Systems/AudioSystem.h"
#include "Systems/RenderSystem.h"
#include "Utils/Logger.h"
#include "VN/VNChoiceList.h"
#include "VN/VNScriptParser.h"

namespace PrismatiX {
namespace App {

class Engine {
public:
    Engine();
    ~Engine();

    bool Initialize(const std::string& title, int width, int height);
    void Clean();
    void Quit() { isRunning = false; }

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

    // Services Accessors (ServiceContainer)
    Services::ResourceManager& GetResourceManager() { return *serviceContainer.Resolve<Services::ResourceManager>(); }
    VN::VNScriptParser& GetScriptingEngine() { return *serviceContainer.Resolve<VN::VNScriptParser>(); }
    Services::GameState& GetGameState() { return *serviceContainer.Resolve<Services::GameState>(); }
    Systems::AudioSystem& GetAudioSystem() { return *serviceContainer.Resolve<Systems::AudioSystem>(); }
    Systems::RenderSystem& GetRenderSystem() { return *serviceContainer.Resolve<Systems::RenderSystem>(); }
    UI::VNChoiceList& GetChoiceList() { return *serviceContainer.Resolve<UI::VNChoiceList>(); }

    ServiceContainer& GetContainer() { return serviceContainer; }

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
    ServiceContainer serviceContainer;

    // Core Services
    std::unique_ptr<Services::ResourceManager> resourceManager;
    std::unique_ptr<VN::VNScriptParser> scriptingEngine;
    std::unique_ptr<Services::GameState> gameState;

    // Systems
    std::unique_ptr<Systems::AudioSystem> audioSystem;
    std::unique_ptr<Systems::RenderSystem> renderSystem;
    std::unique_ptr<UI::VNChoiceList> choiceList;
};

}  // namespace App
}  // namespace PrismatiX
