#include "Engine.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <iostream>

#include "Core/LuaBindings.h"
#include "Core/Services/GameState.h"
#include "Core/Services/ResourceManager.h"
#include "Core/Services/SaveManager.h"
#include "Core/Systems/AudioSystem.h"
#include "Core/Systems/RenderSystem.h"
#include "Core/VN/VNChoiceList.h"
#include "Utils/Logger.h"

#pragma execution_character_set("utf-8")

namespace PrismatiX {
namespace App {

Engine::Engine() : isRunning(false), window(nullptr), renderer(nullptr), leftClick(false), rightClick(false), mouseWheelY(0) {}

Engine::~Engine() { Clean(); }

bool Engine::Initialize(const std::string& title, int width, int height, const InitOptions& options) {
    PX_LOG_INFO("Engine initializing ({}x{})...", width, height);
    logicalWidth = width;
    logicalHeight = height;
    ownWindow = options.ownWindow;
    ownRenderer = options.ownRenderer;
    ownSubsystems = options.ownSubsystems;
    applyRendererLogicalSize = options.applyRendererLogicalSize;
    subsystemsActive = true;

    const Uint32 requiredSdlFlags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS;
    const Uint32 missingSdlFlags = requiredSdlFlags & ~SDL_WasInit(requiredSdlFlags);
    if (missingSdlFlags != 0 && SDL_Init(missingSdlFlags) < 0) {
        PX_LOG_CRITICAL("SDL_Init failed: {}", SDL_GetError());
        return false;
    }

    const int imgFlags = IMG_INIT_PNG | IMG_INIT_JPG;
    const int missingImgFlags = imgFlags & ~IMG_Init(0);
    if (missingImgFlags != 0 && (IMG_Init(missingImgFlags) & missingImgFlags) != missingImgFlags) {
        PX_LOG_CRITICAL("IMG_Init failed: {}", IMG_GetError());
        return false;
    }

    if (!TTF_WasInit() && TTF_Init() < 0) {
        PX_LOG_CRITICAL("TTF_Init failed: {}", TTF_GetError());
        return false;
    }

    const int mixFlags = MIX_INIT_MP3;
    const int missingMixFlags = mixFlags & ~Mix_Init(0);
    if (missingMixFlags != 0 && (Mix_Init(missingMixFlags) & missingMixFlags) != missingMixFlags) {
        PX_LOG_CRITICAL("Mix_Init failed: {}", Mix_GetError());
        return false;
    }

    if (Mix_QuerySpec(nullptr, nullptr, nullptr) == 0) {
        if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0 && Mix_QuerySpec(nullptr, nullptr, nullptr) == 0) {
            PX_LOG_CRITICAL("Mix_OpenAudio failed: {}", Mix_GetError());
            return false;
        }
    }
    subsystemsActive = true;

    if (options.externalWindow && options.externalRenderer) {
        window = options.externalWindow;
        renderer = options.externalRenderer;
        ownWindow = false;
        ownRenderer = false;
    } else {
        PX_LOG_TRACE("Creating SDL Window...");
        window = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!window) return false;

        PX_LOG_TRACE("Creating SDL Renderer...");
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
        if (!renderer) return false;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");

    PX_LOG_INFO("Initializing Engine Services...");
    resourceManager = std::make_unique<Services::ResourceManager>(renderer);
    gameState = std::make_unique<Services::GameState>();
    saveManager = std::make_unique<Services::SaveManager>(*gameState);

    PX_LOG_INFO("Initializing Engine Systems...");
    renderSystem = std::make_unique<Systems::RenderSystem>(renderer, *resourceManager);
    audioSystem = std::make_unique<Systems::AudioSystem>(*resourceManager);
    choiceList = std::make_unique<VN::VNChoiceList>();

    if (applyRendererLogicalSize) {
        SDL_RenderSetLogicalSize(renderer, width, height);
    }

    PX_LOG_DEBUG("Binding Engine to Lua state...");
    this->BindEngineToLua();

    isRunning = true;
    PX_LOG_INFO("Engine initialization complete.");
    return true;
}

void Engine::HandleEvents() {
    int winX, winY;
    SDL_GetMouseState(&winX, &winY);
    if (applyRendererLogicalSize) {
        float lx = 0.0f;
        float ly = 0.0f;
        SDL_RenderWindowToLogical(renderer, winX, winY, &lx, &ly);
        mouseX = static_cast<int>(lx);
        mouseY = static_cast<int>(ly);
    } else if (window) {
        int windowWidth = 0;
        int windowHeight = 0;
        SDL_GetWindowSize(window, &windowWidth, &windowHeight);
        if (windowWidth > 0 && windowHeight > 0) {
            mouseX = static_cast<int>((static_cast<float>(winX) / static_cast<float>(windowWidth)) * static_cast<float>(logicalWidth));
            mouseY = static_cast<int>((static_cast<float>(winY) / static_cast<float>(windowHeight)) * static_cast<float>(logicalHeight));
        }
    }

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

void Engine::InjectInputState(int x, int y, bool left, bool right, int wheelY) {
    mouseX = x;
    mouseY = y;
    leftClick = left;
    rightClick = right;
    mouseWheelY = wheelY;
}

void Engine::ClearScreen() {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
}

void Engine::PresentScreen() { SDL_RenderPresent(renderer); }

void Engine::SetCameraOffset(int x, int y) {
    if (renderSystem) renderSystem->SetCameraOffset(x, y);
}
void Engine::ResetCameraOffset() {
    if (renderSystem) renderSystem->SetCameraOffset(0, 0);
}

// Accessor definitions
PrismatiX::Services::ResourceManager& Engine::GetResourceManager() { return *resourceManager; }
PrismatiX::Services::GameState& Engine::GetGameState() { return *gameState; }
PrismatiX::Services::SaveManager& Engine::GetSaveManager() { return *saveManager; }
PrismatiX::Systems::AudioSystem& Engine::GetAudioSystem() { return *audioSystem; }
PrismatiX::Systems::RenderSystem& Engine::GetRenderSystem() { return *renderSystem; }
PrismatiX::VN::VNChoiceList& Engine::GetChoiceList() { return *choiceList; }

void Engine::Clean() {
    PX_LOG_INFO("Engine shutting down...");
    if (resourceManager) resourceManager->CleanAll();
    if (choiceList) choiceList->Clear();
    if (ownRenderer && renderer) SDL_DestroyRenderer(renderer);
    if (ownWindow && window) SDL_DestroyWindow(window);
    if (ownSubsystems && subsystemsActive) {
        Mix_CloseAudio();
        Mix_Quit();
        TTF_Quit();
        IMG_Quit();
        SDL_Quit();
    }
    isRunning = false;
    renderer = nullptr;
    window = nullptr;
    resourceManager.reset();
    gameState.reset();
    saveManager.reset();
    audioSystem.reset();
    renderSystem.reset();
    choiceList.reset();
    subsystemsActive = false;
    PX_LOG_INFO("Engine resources released.");
}

void Engine::BindEngineToLua() { RegisterEngineLuaBindings(lua, *this); }

}  // namespace App
}  // namespace PrismatiX
