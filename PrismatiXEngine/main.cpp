#include <iostream>
#include "PrismatiXEngine.h"
#include "Managers/TextureManager.h"
#include "Managers/TextManager.h"
#include "DialogueBox.h"
#include "Controllers/DialogueController.h"

#pragma execution_character_set("utf-8") // 防中文亂碼

enum class GameState {
    MainMenu,
    InGame
};

int main(int argc, char* argv[]) {
    PrismatiXEngine engine;
    if (!engine.Initialize("PrismatiX Visual Novel Engine", 1280, 720)) {
        return -1;
    }

    SDL_Renderer* renderer = engine.GetRenderer();

    SDL_Texture* bg = TextureManager::LoadTexture("bg.jpg", renderer);
    TTF_Font* font = TextManager::LoadFont("NotoSansTC-Bold.ttf", 28);
    DialogueBox dialog(font, 50, 600);
    DialogueController vnController(&dialog, renderer);

    SDL_Texture* titleBg = TextureManager::LoadTexture("title_bg.jpg", renderer);
    std::vector<VNCommand> script;
    GameState currentState = GameState::MainMenu;

    while (engine.IsRunning()) {
        engine.HandleEvents();

        engine.ClearScreen();

        if (currentState == GameState::MainMenu) {
            if (titleBg) {
                engine.DrawFullscreenBackground(titleBg);
            }
        }
        else if (currentState == GameState::InGame) {
            vnController.RenderBackground(engine);
        }


        engine.BeginSafeArea();

        if (currentState == GameState::MainMenu) {
            SDL_Color textColor = { 255, 255, 255, 255 };
            SDL_Color outlineColor = { 0, 0, 0, 255 };
            TextManager::DrawWithOutline(renderer, font, "(Click to Start)", textColor, outlineColor, 2, 400, 550);
          

            if (engine.GetLeftClick()) {
                script = ScriptManager::ParseFile("script.pes");
                vnController.LoadScript(script);
                currentState = GameState::InGame;
            }
        }
        else if (currentState == GameState::InGame) {
            if (engine.GetLeftClick() || engine.GetMouseWheelY() < 0) {
                vnController.HandleClick();
            }

            vnController.Update();
            vnController.Render(renderer);
        }

        engine.EndSafeArea();
        engine.PresentScreen();
    }

    engine.Clean();
    return 0;
}