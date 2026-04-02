#include "PrismatiXEngine.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <iostream>

#include "LuaBindings.h"

#pragma execution_character_set("utf-8")  // 防中文亂碼

PrismatiXEngine::PrismatiXEngine()
    : isRunning(false),
      window(nullptr),
      renderer(nullptr),
      leftClick(false),
      rightClick(false),
      mouseWheelY(0),
      assetManager(archiveManager),
      audioSystem(assetManager),
      saveManager(variableManager, backlogManager),
      scriptManager(archiveManager),
      uiManager(renderSystem) {}
PrismatiXEngine::~PrismatiXEngine() { Clean(); }

bool PrismatiXEngine::Initialize(const std::string& title, int width, int height) {  // 同標頭檔裡面的解釋
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) < 0) {           // 其實應該可以不用加 Events
        std::cerr << "Failed to initialize SDL2: " << SDL_GetError() << std::endl;
        return false;
    }

    int imgFlags = IMG_INIT_PNG | IMG_INIT_JPG;

    // 幹 SDL2 能不能統一啊為什麼只有 image 是 bool 啊
    if (!(IMG_Init(imgFlags) & imgFlags)) {
        std::cerr << "Failed to initialize SDL2_image: " << IMG_GetError() << std::endl;
        return false;
    }

    if (TTF_Init() < 0) {
        std::cerr << "Failed to initialize SDL2_ttf: " << TTF_GetError() << std::endl;
        return false;
    }

    if (!(Mix_Init(MIX_INIT_MP3) & MIX_INIT_MP3)) {
        std::cerr << "Failed to initialize SDL2_mixer: " << Mix_GetError() << std::endl;
        return false;
    }

    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
        std::cerr << "Failed to open SDL2_mixer audio: " << Mix_GetError() << std::endl;
        return false;
    }

    window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);  // 他不吃 std::string 所以轉 c_str
    if (!window) {
        std::cerr << "Failed to create window: " << SDL_GetError() << std::endl;
        return false;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");  // 改用 Linear Filtering

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);  // 硬體加速 & VSync
    if (!renderer) {
        std::cerr << "Failed to create renderer: " << SDL_GetError() << std::endl;
        return false;
    }

    renderSystem = std::make_unique<RenderSystem>(renderer, assetManager);

    SDL_RenderSetLogicalSize(renderer, width, height);
    lastWinW = width;
    lastWinH = height;

    this->BindEngineToLua();

    isRunning = true;
    return true;
}

void PrismatiXEngine::HandleEvents() {
    int winX, winY;
    SDL_GetMouseState(&winX, &winY);
    float logicalX, logicalY;
    SDL_RenderWindowToLogical(renderer, winX, winY, &logicalX, &logicalY);
    mouseX = static_cast<int>(logicalX);
    mouseY = static_cast<int>(logicalY);

    leftClick = false;
    rightClick = false;
    mouseWheelY = 0;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            isRunning = false;
        }
        else if (event.type == SDL_WINDOWEVENT) {
            // 不用了，當時我到底在幹嘛，算了先這樣
        }
        else if (event.type == SDL_MOUSEBUTTONDOWN) {
            if (event.button.button == SDL_BUTTON_LEFT) {
                leftClick = true;
            }
            else if (event.button.button == SDL_BUTTON_RIGHT) {
                rightClick = true;
            }
        }
        else if (event.type == SDL_MOUSEWHEEL) {
            mouseWheelY = event.wheel.y;
        }
    }
}

void PrismatiXEngine::ClearScreen() {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
}

void PrismatiXEngine::PresentScreen() { SDL_RenderPresent(renderer); }

void PrismatiXEngine::SetCameraOffset(int x, int y) {
    cameraOffsetX = x;
    cameraOffsetY = y;
}

void PrismatiXEngine::ResetCameraOffset() {
    cameraOffsetX = 0;
    cameraOffsetY = 0;
}

void PrismatiXEngine::ApplyCameraViewport() {
    // 何意味
}

void PrismatiXEngine::Clean() {
    assetManager.CleanAll();
    uiManager.Clear();

    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);

    Mix_CloseAudio();
    Mix_Quit();
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();

    std::cout << "Application destroyed." << std::endl;
}

void PrismatiXEngine::DrawFullscreenBackground(SDL_Texture* bgTex, Uint8 alpha) {
    if (!bgTex || !renderSystem) return;
    renderSystem->DrawTextureAuto(bgTex, DisplayMode::Fill, alpha);
}

void PrismatiXEngine::BindEngineToLua() { RegisterEngineLuaBindings(lua, *this); }