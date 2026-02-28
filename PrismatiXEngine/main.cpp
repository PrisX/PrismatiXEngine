#include <iostream>
#include "PrismatiXEngine.h"
#include "Managers/TextureManager.h"
#include "Managers/TextManager.h"
#include "Managers/AudioManager.h"
#include "Managers/ScriptManager.h"
#include "DialogueBox.h"
#include "Controllers/DialogueController.h"
#include "Controllers/SplashController.h"
#include "Managers/ArchiveManager.h"
#include "Managers/ConfigManager.h"

#pragma execution_character_set("utf-8") // 防中文亂碼

enum class GameState {
    SplashScreen,
    MainMenu,
    InGame
};

int main(int argc, char* argv[]) {


    ArchiveManager::MountArchive("Data/Game.pdx");
    if (!ConfigManager::LoadConfig("Game")) {
        std::cerr << "Entry point not found or invalid." << std::endl;
        return -1;
    }

    std::string gameTitle = ConfigManager::GetString("Title", "PrismatiX Engine");
    int winW = ConfigManager::GetInt("Width", 1280);
    int winH = ConfigManager::GetInt("Height", 720);

    PrismatiXEngine engine;
    if (!engine.Initialize(gameTitle, winW, winH)) {
        return -1;
    }

    SDL_Renderer* renderer = engine.GetRenderer();

    ArchiveManager::MountArchive("Data/Image/Character.pdi");
    ArchiveManager::MountArchive("Data/Image/Background.pdi");
    ArchiveManager::MountArchive("Data/Image/UI.pdi");

    ArchiveManager::MountArchive("Data/Font.pdttf");
    ArchiveManager::MountArchive("Data/Audio/Music.pda");
    ArchiveManager::MountArchive("Data/Audio/SFX.pda");
    ArchiveManager::MountArchive("Data/Audio/Voice.pda");
    ArchiveManager::MountArchive("Data/Script/script.pds");

    std::string fontName = ConfigManager::GetString("FontName", "NotoSansTC-Bold.ttf");
    int fontSize = ConfigManager::GetInt("FontSize", 28);
    TTF_Font* font = TextManager::LoadFont(fontName, fontSize);
    DialogueBox dialog(font, 50, winH - 120);
    DialogueController vnController(&dialog, renderer);

    std::string initBgFile = ConfigManager::GetString("InitialBg", "title_bg.jpg");
    SDL_Texture* titleBg = TextureManager::LoadTexture("title_bg.jpg", renderer);

    std::vector<VNCommand> script;
    GameState currentState = GameState::SplashScreen;
    SplashController splashController;
    splashController.Init(renderer);

    float mainMenuAlpha = 0.0f;
    const float MAIN_MENU_FADE_SPEED = 3.0f;
    while (engine.IsRunning()) {
        engine.HandleEvents();

        engine.ClearScreen();
        if (currentState == GameState::SplashScreen) {
            splashController.Update();
            splashController.Render(renderer, winW, winH);

            if (splashController.IsFinished()) {
                currentState = GameState::MainMenu;
                mainMenuAlpha = 0.0f;
            }
        }
        else if (currentState == GameState::MainMenu) {
            if (mainMenuAlpha < 255.0f) {
                mainMenuAlpha += MAIN_MENU_FADE_SPEED;
                if (mainMenuAlpha > 255.0f) mainMenuAlpha = 255.0f;
            }

            if (titleBg) {
                engine.DrawFullscreenBackground(titleBg, (Uint8)mainMenuAlpha);
            }
        }
        else if (currentState == GameState::InGame) {
            vnController.RenderBackground(engine);
        }


        engine.BeginSafeArea();

        if (currentState == GameState::SplashScreen) {
            if (engine.GetLeftClick()) {
                splashController.HandleClick();
            }
        }
        else if (currentState == GameState::MainMenu) {
            SDL_Color textColor = { 255, 255, 255, 255 };
            SDL_Color outlineColor = { 0, 0, 0, 255 };
            TextManager::DrawWithOutline(renderer, font, "(Click to Start)", textColor, outlineColor, 2, 400, 550);
          

            if (engine.GetLeftClick()) {
                std::string startScriptFile = ConfigManager::GetString("StartScript", "script");
                script = ScriptManager::ParseFile(startScriptFile);
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

    TextManager::Clean();
    AudioManager::CleanCache();
    engine.Clean();
    return 0;
}