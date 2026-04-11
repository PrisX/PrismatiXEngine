#pragma once

#include <SDL2/SDL.h>

#include <lua.hpp>
#include <memory>
#include <cstddef>
#include <sol/sol.hpp>
#include <string>

#include "EngineConfig.h"
#include "Services/GameState.h"
#include "Services/ResourceManager.h"
#include "Services/SaveManager.h"
#include "Systems/AudioSystem.h"
#include "Systems/RenderSystem.h"
#include "Utils/Logger.h"
#include "VN/VNChoiceList.h"

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

    // Services Accessors
    PrismatiX::Services::ResourceManager& GetResourceManager() { return *resourceManager; }
    PrismatiX::Services::GameState& GetGameState() { return *gameState; }
    PrismatiX::Services::SaveManager& GetSaveManager() { return *saveManager; }
    PrismatiX::Systems::AudioSystem& GetAudioSystem() { return *audioSystem; }
    PrismatiX::Systems::RenderSystem& GetRenderSystem() { return *renderSystem; }
    PrismatiX::VN::VNChoiceList& GetChoiceList() { return *choiceList; }

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
    std::unique_ptr<PrismatiX::Services::ResourceManager> resourceManager;
    std::unique_ptr<PrismatiX::Services::GameState> gameState;
    std::unique_ptr<PrismatiX::Services::SaveManager> saveManager;

    // Systems
    std::unique_ptr<PrismatiX::Systems::AudioSystem> audioSystem;
    std::unique_ptr<PrismatiX::Systems::RenderSystem> renderSystem;
    std::unique_ptr<PrismatiX::VN::VNChoiceList> choiceList;
};

}  // namespace App
}  // namespace PrismatiX
