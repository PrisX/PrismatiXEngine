#pragma once

#include <iostream>
#include <SDL2/SDL.h>
#include <string>

#include "TextureManager.h"
#include "TextManager.h"
#include "AudioManager.h"

#include "DialogueBox.h"

class PrismatiXEngine {
public:
    // Constructor & Destructor
    PrismatiXEngine();
    ~PrismatiXEngine();

    // Lifecycle
    bool Initialize(const std::string& title, int width, int height); // 雖然這裡應該是沒什麼差，但是為了 Consistency 還是傳址吧
    void Clean();

    bool IsRunning() const { return isRunning; }
    SDL_Renderer* GetRenderer() const { return renderer; }
    SDL_Window* GetWindow() const { return window; }
    bool GetLeftClick() const { return leftClick; }
    int GetMouseWheelY() const { return mouseWheelY; }

    void HandleEvents();
    void ClearScreen();
    void PresentScreen();

    void DrawFullscreenBackground(SDL_Texture* bgTex, Uint8 alpha = 255);
    void BeginSafeArea();
    void EndSafeArea();
private:
    bool leftClick;
    int mouseWheelY;
    bool isRunning;

    // Long-term resources
    SDL_Window* window;
    SDL_Renderer* renderer;
};