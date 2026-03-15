#pragma once

#include <SDL2/SDL.h>

#include <iostream>
#include <lua.hpp>
#include <sol/sol.hpp>
#include <string>

#include "AudioManager.h"
#include "TextManager.h"
#include "TextureManager.h"

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
    void HandleEvents();
    void ClearScreen();
    void PresentScreen();

    void DrawFullscreenBackground(SDL_Texture* bgTex, Uint8 alpha = 255);

    void BindEngineToLua();
    sol::state& GetLuaState() { return lua; }

private:
    int lastWinW = 1280;
    int lastWinH = 720;
    bool leftClick;
    bool rightClick;
    int mouseWheelY;
    bool isRunning;
    int mouseX = 0;
    int mouseY = 0;

    // Long-term resources
    SDL_Window* window;
    SDL_Renderer* renderer;
    sol::state lua;
};