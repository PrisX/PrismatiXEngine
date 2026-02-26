#include <iostream>
#include "PrismatiXEngine.h"
#include "Managers/TextureManager.h"
#include "Managers/TextManager.h"
#include "DialogueBox.h"
#include "Controllers/DialogueController.h"

#pragma execution_character_set("utf-8") // 防中文亂碼

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

    std::vector<VNCommand> s = ScriptManager::ParseFile("script.pes");
    vnController.LoadScript(s);

    while (engine.IsRunning()) {
        engine.HandleEvents();
        if (engine.GetLeftClick()) {
            vnController.HandleClick();
        }

        if (engine.GetLeftClick() || engine.GetMouseWheelY() < 0) {
            vnController.HandleClick();
        }

        else if (engine.GetMouseWheelY() > 0) {
            // TODO: backlog
        }

		vnController.Update();

        engine.ClearScreen();

        engine.DrawFullscreenBackground(bg);

        engine.BeginSafeArea();

        vnController.Render(renderer);

        engine.EndSafeArea();

        engine.PresentScreen();
    }

    engine.Clean();
    return 0;
}