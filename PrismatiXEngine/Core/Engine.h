#pragma once

#include <SDL2/SDL.h>

#include <memory>
#include <sol/sol.hpp>
#include <string>

#include "EngineConfig.h"

namespace PrismatiX::Services {
class ResourceManager;
class GameState;
class SaveManager;
}  // namespace PrismatiX::Services
namespace PrismatiX::Systems {
class AudioSystem;
class RenderSystem;
}  // namespace PrismatiX::Systems
namespace PrismatiX::VN {
class VNChoiceList;
}

namespace PrismatiX::App {

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

    // Service accessors
    PrismatiX::Services::ResourceManager& GetResourceManager();
    PrismatiX::Services::GameState& GetGameState();
    PrismatiX::Services::SaveManager& GetSaveManager();
    PrismatiX::Systems::AudioSystem& GetAudioSystem();
    PrismatiX::Systems::RenderSystem& GetRenderSystem();
    PrismatiX::VN::VNChoiceList& GetChoiceList();

private:
    int lastWinW = EngineConfig::kDefaultScreenWidth;
    int lastWinH = EngineConfig::kDefaultScreenHeight;
    bool leftClick = false;
    bool rightClick = false;
    int mouseWheelY = 0;
    bool isRunning = false;
    int mouseX = 0;
    int mouseY = 0;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    sol::state lua;

    std::unique_ptr<PrismatiX::Services::ResourceManager> resourceManager;
    std::unique_ptr<PrismatiX::Services::GameState> gameState;
    std::unique_ptr<PrismatiX::Services::SaveManager> saveManager;
    std::unique_ptr<PrismatiX::Systems::AudioSystem> audioSystem;
    std::unique_ptr<PrismatiX::Systems::RenderSystem> renderSystem;
    std::unique_ptr<PrismatiX::VN::VNChoiceList> choiceList;
};

}  // namespace PrismatiX::App
