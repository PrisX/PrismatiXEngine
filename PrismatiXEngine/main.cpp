#include <iostream>
#include "PrismatiXEngine.h"
#include "Managers/TextureManager.h"
#include "Managers/TextManager.h"
#include "DialogueBox.h"

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
    dialog.SetText("這篇文章在隱喻作者懷才不遇，屢遭貶官的抑鬱心情。", 40);

    while (engine.IsRunning()) {
        engine.HandleEvents();

        dialog.Update();

        engine.ClearScreen();

        int winW, winH;
        SDL_GetWindowSize(engine.GetWindow(), &winW, &winH);

        float scaleX = (float)winW / 1280.0f;
        float scaleY = (float)winH / 720.0f;

        float bgScale = std::max(scaleX, scaleY);
        int bgW = 1280 * bgScale;
        int bgH = 720 * bgScale;
        int bgX = (winW - bgW) / 2;
        int bgY = (winH - bgH) / 2;

        if (bg) TextureManager::Draw(bg, renderer, bgX, bgY, bgW, bgH);
        float safeScale = std::min(scaleX, scaleY);
        int safeW = 1280 * safeScale;
        int safeH = 720 * safeScale;
        int safeX = (winW - safeW) / 2;
        int safeY = (winH - safeH) / 2;

        SDL_Rect safeArea = { safeX, safeY, safeW, safeH };
        SDL_RenderSetViewport(renderer, &safeArea);
        SDL_RenderSetScale(renderer, safeScale, safeScale);


        if (girl) TextureManager::Draw(girl, renderer, 420, 120, 0.1f);
        dialog.Render(renderer);

        SDL_RenderSetViewport(renderer, NULL);
        SDL_RenderSetScale(renderer, 1.0f, 1.0f);

        engine.PresentScreen();
    }

    engine.Clean();
    return 0;
}