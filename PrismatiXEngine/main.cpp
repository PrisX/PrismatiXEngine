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


    ArchiveManager::MountArchive("Data.pdx");
    if (!ConfigManager::LoadConfig("Game")) {
        std::cerr << "Entry point not found or invalid." << std::endl;
        return -1;
    }

    std::string mountList = ConfigManager::GetString("Mount", "Data.pdx");

    std::stringstream ss(mountList);
    std::string pdxName;
    while (std::getline(ss, pdxName, ',')) {
        pdxName.erase(0, pdxName.find_first_not_of(" \t\r\n"));
        pdxName.erase(pdxName.find_last_not_of(" \t\r\n") + 1);

        if (!pdxName.empty()) {
            ArchiveManager::MountArchive(pdxName);
        }
    }

    std::string gameTitle = ConfigManager::GetString("Title", "PrismatiX Engine");
    int winW = ConfigManager::GetInt("Width", 1280);
    int winH = ConfigManager::GetInt("Height", 720);

    PrismatiXEngine engine;
    if (!engine.Initialize(gameTitle, winW, winH)) {
        return -1;
    }

    SDL_Renderer* renderer = engine.GetRenderer();


    std::string fontName = ConfigManager::GetString("FontName", "NotoSansTC-Bold.ttf");
    int fontSize = ConfigManager::GetInt("FontSize", 28);
    TTF_Font* font = TextManager::LoadFont(fontName, fontSize);
    DialogueBox dialog(font, 50, winH - 120);
    DialogueController vnController(&dialog, font, renderer);

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
            int wheelY = engine.GetMouseWheelY();
            if (vnController.IsShowingBacklog()) {
                if (wheelY != 0) {
                    vnController.ScrollBacklog(wheelY > 0 ? 1 : -1);
                }

                if (engine.GetRightClick()) {
                    vnController.ToggleBacklog();
                }
            }

            else {
                if (engine.GetLeftClick() || wheelY < 0) {
                    vnController.HandleClick();
                }
                else if (wheelY > 0) {
                    vnController.ToggleBacklog();
                }
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