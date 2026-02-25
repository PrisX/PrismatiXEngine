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
    SDL_Texture* girl = TextureManager::LoadTexture("girl.png", renderer);
    TTF_Font* font = TextManager::LoadFont("NotoSansTC-Bold.ttf", 28);
    DialogueBox dialog(font, 50, 600);
	DialogueController dialogueController(&dialog);

    dialogueController.LoadScript({
        {"owen", "Illya my wife!"},
        {"", "一般的旁白"},
        {"", "一般的旁白"},
        {"", "一般的旁白"},
        {"", "一般的旁白"},
        {"", "一般的旁白"},
        {"", "一般的旁白"},
        {"", "一般的旁白"},
        {"", "一般的旁白"},
        {"", "一般的旁白"},
        {"", "一般的旁白"},
        {"", "一般的旁白"},
        {"", "一般的旁白"},
        });

    while (engine.IsRunning()) {
        engine.HandleEvents();
        if (engine.GetLeftClick()) {
            dialogueController.HandleClick();
        }

        if (engine.GetLeftClick() || engine.GetMouseWheelY() < 0) {
            dialogueController.HandleClick();
        }

        else if (engine.GetMouseWheelY() > 0) {
            // TODO: backlog
        }

        dialog.Update();
		dialogueController.Update();

        engine.ClearScreen();

        engine.DrawFullscreenBackground(bg);

        engine.BeginSafeArea();

        if (girl) TextureManager::Draw(girl, renderer, 420, 120, 0.1f);
        dialog.Render(renderer);

        engine.EndSafeArea();

        engine.PresentScreen();
    }

    engine.Clean();
    return 0;
}