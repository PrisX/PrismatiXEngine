#include "Engine.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <iostream>

#include "Core/LuaBindings.h"
#include "Utils/Logger.h"

#pragma execution_character_set("utf-8")

Engine::Engine() : isRunning(false), window(nullptr), renderer(nullptr), leftClick(false), rightClick(false), mouseWheelY(0) {}

Engine::~Engine() { Clean(); }

bool Engine::Initialize(const std::string& title, int width, int height) {
    PX_LOG_INFO("Engine initializing ({}x{})...", width, height);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) < 0) {
        PX_LOG_CRITICAL("SDL_Init failed: {}", SDL_GetError());
        return false;
    }

    int imgFlags = IMG_INIT_PNG | IMG_INIT_JPG;
    if (!(IMG_Init(imgFlags) & imgFlags)) {
        PX_LOG_CRITICAL("IMG_Init failed: {}", IMG_GetError());
        return false;
    }

    if (TTF_Init() < 0) {
        PX_LOG_CRITICAL("TTF_Init failed: {}", TTF_GetError());
        return false;
    }

    if (!(Mix_Init(MIX_INIT_MP3) & MIX_INIT_MP3)) {
        PX_LOG_CRITICAL("Mix_Init failed: {}", Mix_GetError());
        return false;
    }

    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
        PX_LOG_CRITICAL("Mix_OpenAudio failed: {}", Mix_GetError());
        return false;
    }

    PX_LOG_TRACE("Creating SDL Window...");
    window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) return false;

    PX_LOG_TRACE("Creating SDL Renderer...");
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) return false;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    PX_LOG_INFO("Initializing Engine Services...");
    resourceManager = std::make_unique<ResourceManager>(renderer);
    scriptingEngine = std::make_unique<ScriptingEngine>(*resourceManager);
    gameState = std::make_unique<GameState>();

    PX_LOG_INFO("Initializing Engine Systems...");
    renderSystem = std::make_unique<RenderSystem>(renderer, *resourceManager);
    audioSystem = std::make_unique<AudioSystem>(*resourceManager);
    uiManager = std::make_unique<UIManager>(*renderSystem);

    SDL_RenderSetLogicalSize(renderer, width, height);

    PX_LOG_DEBUG("Binding Engine to Lua state...");
    this->BindEngineToLua();

    isRunning = true;
    PX_LOG_INFO("Engine initialization complete.");
    return true;
}

void Engine::HandleEvents() {
    int winX, winY;
    SDL_GetMouseState(&winX, &winY);
    float lx, ly;
    SDL_RenderWindowToLogical(renderer, winX, winY, &lx, &ly);
    mouseX = (int)lx;
    mouseY = (int)ly;

    leftClick = false;
    rightClick = false;
    mouseWheelY = 0;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            isRunning = false;
        else if (event.type == SDL_MOUSEBUTTONDOWN) {
            if (event.button.button == SDL_BUTTON_LEFT)
                leftClick = true;
            else if (event.button.button == SDL_BUTTON_RIGHT)
                rightClick = true;
        }
        else if (event.type == SDL_MOUSEWHEEL)
            mouseWheelY = event.wheel.y;
    }
}

void Engine::ClearScreen() {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
}

void Engine::PresentScreen() { SDL_RenderPresent(renderer); }

void Engine::SetCameraOffset(int x, int y) {
    cameraOffsetX = x;
    cameraOffsetY = y;
}
void Engine::ResetCameraOffset() {
    cameraOffsetX = 0;
    cameraOffsetY = 0;
}
void Engine::ApplyCameraViewport() {}

void Engine::Clean() {
    PX_LOG_INFO("Engine shutting down...");
    if (resourceManager) resourceManager->CleanAll();
    if (uiManager) uiManager->Clear();
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    Mix_CloseAudio();
    Mix_Quit();
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    PX_LOG_INFO("Engine resources released.");
}

void Engine::BindEngineToLua() { RegisterEngineLuaBindings(lua, *this); }
