#pragma once

#include <iostream>
#include <SDL2/SDL.h>
#include <string>
#include "TextureManager.h"
#include "TextManager.h"

class PrismatiXEngine {
public:
    // Constructor & Destructor
    PrismatiXEngine();
    ~PrismatiXEngine();

    // Lifecycle
    bool Initialize(const std::string& title, int width, int height); // 雖然這裡應該是沒什麼差，但是為了 Consistency 還是傳址吧
    void Run();
    void Clean();

private:
    void HandleEvents();
    void Update();
    void Render(); 

    bool isRunning;

    // Long-term resources
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* backgroundTex;
	SDL_Texture* characterTex;
    TTF_Font* mainFont;
};