#pragma once

#include <SDL2/SDL.h>

#include <iostream>
#include <lua.hpp>
#include <sol/sol.hpp>
#include <string>

#include "Managers/ArchiveManager.h"
#include "Managers/AudioManager.h"
#include "Managers/BacklogManager.h"
#include "Managers/SaveManager.h"
#include "Managers/ScriptManager.h"
#include "Managers/TextManager.h"
#include "Managers/TextureManager.h"
#include "Managers/UIManager.h"
#include "Managers/VariableManager.h"

#include "EngineConfig.h"

class PrismatiXEngine {
public:
    // Constructor & Destructor
    PrismatiXEngine();
    ~PrismatiXEngine();

    // Lifecycle
    bool Initialize(const std::string& title, int width, int height);  // 雖然這裡應該是沒什麼差，但是為了 Consistency 還是傳址吧
    void Clean();

    bool IsRunning() const { return isRunning; }
    SDL_Renderer* GetRenderer() const { return renderer; }
    SDL_Window* GetWindow() const { return window; }
    bool GetLeftClick() const { return leftClick; }
    bool GetRightClick() const { return rightClick; }
    int GetMouseWheelY() const { return mouseWheelY; }
    int GetMouseX() const { return mouseX; }
    int GetMouseY() const { return mouseY; }
    void SetCameraOffset(int x, int y);
    void ResetCameraOffset();
    void HandleEvents();
    void ClearScreen();
    void PresentScreen();

    void DrawFullscreenBackground(SDL_Texture* bgTex, Uint8 alpha = 255);

    void BindEngineToLua();
    sol::state& GetLuaState() { return lua; }

    // Managers Accessors
    ArchiveManager& GetArchiveManager() { return archiveManager; }
    AudioManager& GetAudioManager() { return audioManager; }
    TextureManager& GetTextureManager() { return textureManager; }
    TextManager& GetTextManager() { return textManager; }
    VariableManager& GetVariableManager() { return variableManager; }
    BacklogManager& GetBacklogManager() { return backlogManager; }
    SaveManager& GetSaveManager() { return saveManager; }
    ScriptManager& GetScriptManager() { return scriptManager; }
    UIManager& GetUIManager() { return uiManager; }

private:
    int lastWinW = EngineConfig::kDefaultScreenWidth;
    int lastWinH = EngineConfig::kDefaultScreenHeight;
    bool leftClick;
    bool rightClick;
    int mouseWheelY;
    bool isRunning;
    int mouseX = 0;
    int mouseY = 0;
    int cameraOffsetX = 0;
    int cameraOffsetY = 0;

    // Long-term resources
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    sol::state lua;

    void ApplyCameraViewport();

    // Manager Instances (Order determines initialization sequence)
    ArchiveManager archiveManager;
    AudioManager audioManager;
    TextureManager textureManager;
    TextManager textManager;
    VariableManager variableManager;
    BacklogManager backlogManager;
    SaveManager saveManager;
    ScriptManager scriptManager;
    UIManager uiManager;
};