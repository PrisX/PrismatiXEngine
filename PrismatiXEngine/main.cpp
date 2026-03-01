#include <iostream>
#include <sstream>
#include <functional>
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
#include "Toolbar.h"
#include "SaveLoadMenu.h"
#include "MainMenu.h"

#pragma execution_character_set("utf-8") // 防中文亂碼

enum class GameState {
    SplashScreen,
    MainMenu,
    InGame,
    SaveMenu,
    LoadMenu
};

int main(int argc, char* argv[]) {

    if (!ArchiveManager::MountArchive("Engine.pdx")) {
		std::cerr << "Failed to mount engine assets." << std::endl;
		return -1;
    }
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
    Toolbar bottomToolbar(font, winH);
    SaveLoadMenu slMenu(font);
    MainMenu titleMenu(font, winW, winH);
    std::string initBgFile = ConfigManager::GetString("InitialBg", "title_bg.jpg");
    SDL_Texture* titleBg = TextureManager::LoadTexture("title_bg.jpg", renderer);

    std::vector<VNCommand> script;
    GameState currentState = GameState::SplashScreen;
    GameState previousState = GameState::MainMenu;
    SplashController splashController;
    splashController.Init(renderer);

    float fadeAlpha = 0.0f;
    bool isFadingOut = false;
    bool isFadingIn = false;
    std::function<void()> pendingFadeAction;
    const float SCREEN_FADE_SPEED = 8.0f;

    auto StartFade = [&](std::function<void()> action) {
        if (isFadingOut || isFadingIn) return;
        isFadingOut = true;
        fadeAlpha = 0.0f;
        pendingFadeAction = std::move(action);
    };

    float mainMenuAlpha = 0.0f;
    const float MAIN_MENU_FADE_SPEED = 3.0f;
    while (engine.IsRunning()) {
        engine.HandleEvents();
        bool isFading = isFadingOut || isFadingIn;

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



        if (currentState == GameState::SplashScreen) {
            if (engine.GetLeftClick()) {
                splashController.HandleClick();
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

                int mx = engine.GetMouseX();
                int my = engine.GetMouseY();
                bool isLeftClicked = !isFading && engine.GetLeftClick();

                std::string action = titleMenu.Update(mx, my, isLeftClicked);

                if (!isFading) {
                    if (action == "Start") {
                        AudioManager::PlaySFX("click.wav");
                        StartFade([&]() {
                            std::string startScriptFile = ConfigManager::GetString("StartScript", "script");
                            script = ScriptManager::ParseFile(startScriptFile);
                            vnController.LoadScript(startScriptFile, script);
                            currentState = GameState::InGame;
                        });
                    }
                    else if (action == "Load") {
                        AudioManager::PlaySFX("click.wav");
                        StartFade([&]() {
                            previousState = GameState::MainMenu;
                            currentState = GameState::LoadMenu;
                            slMenu.Open(SLMode::Load);
                        });
                    }
                    else if (action == "Exit") {
                        break;
                    }
                }

                titleMenu.Render(renderer);
            }
        else if (currentState == GameState::InGame) {
                int mx = engine.GetMouseX();
                int my = engine.GetMouseY();
                int wheelY = isFading ? 0 : engine.GetMouseWheelY();

                bool isLeftClicked = !isFading && engine.GetLeftClick();

                if (!isFading) {
                    if (vnController.IsShowingBacklog()) {
                        if (wheelY != 0) {
                            vnController.ScrollBacklog(wheelY > 0 ? 1 : -1);
                        }

                        if (engine.GetRightClick() || wheelY < 0) {
                            vnController.ToggleBacklog();
                        }
                    }
                    else {
                        std::string toolbarAction = bottomToolbar.Update(mx, my, isLeftClicked);

                        if (toolbarAction == "OpenSave") {
                            StartFade([&]() {
                                previousState = GameState::InGame;
                                currentState = GameState::SaveMenu;
                                slMenu.Open(SLMode::Save);
                            });
                        }
                        else if (toolbarAction == "OpenLoad") {
                            StartFade([&]() {
                                previousState = GameState::InGame;
                                currentState = GameState::LoadMenu;
                                slMenu.Open(SLMode::Load);
                            });
                        }
                        else if (toolbarAction == "TogglePin") {
                            AudioManager::PlaySFX("click.wav");
                        }

                        bool blockingClick = bottomToolbar.IsMouseOver(my);

                        if ((isLeftClicked && !blockingClick) || wheelY < 0) {
                            vnController.HandleClick(mx, my);
                        }
                        else if (wheelY > 0) {
                            vnController.ToggleBacklog();
                        }
                    }
                }

                vnController.Update(mx, my);
                vnController.Render(renderer);

                bottomToolbar.Render(renderer);
            }
        else if (currentState == GameState::SaveMenu || currentState == GameState::LoadMenu) {
            int mx = engine.GetMouseX();
            int my = engine.GetMouseY();
            bool isLeftClicked = engine.GetLeftClick();

            vnController.RenderBackground(engine);
            vnController.Render(renderer);

            int selectedSlot = !isFading ? slMenu.Update(mx, my, isLeftClicked) : 0;

            if (!isFading) {
                if (selectedSlot == -1 || engine.GetRightClick()) {
                    StartFade([&]() { currentState = previousState; });
                }
                else if (selectedSlot > 0) {
                    if (currentState == GameState::SaveMenu) {
                        SaveManager::SaveGame(selectedSlot, vnController.GetCurrentScriptName(), vnController.GetCurrentLine(), vnController.GetCurrentBgName(), vnController.GetCurrentBgmName(), vnController.GetSavedCharacters());

                        AudioManager::PlaySFX("click.wav");
                        slMenu.Open(SLMode::Save);
                    }
                    else if (currentState == GameState::LoadMenu) {
                        int capturedSlot = selectedSlot;
                        StartFade([&, capturedSlot]() {
                            std::string scriptName, bgName, bgmName;
                            int line = 0;
                            std::vector<SavedCharacter> chars;

                            if (SaveManager::LoadGame(capturedSlot, scriptName, line, bgName, bgmName, chars)) {
                                script = ScriptManager::ParseFile(scriptName);
                                vnController.LoadScript(scriptName, script);
                                vnController.SetCurrentLine(line);

                                vnController.RestoreBackground(bgName);
                                if (!bgmName.empty()) {
                                    AudioManager::PlayBGM(bgmName);
                                }

                                vnController.RestoreSavedCharacters(chars);

                                AudioManager::PlaySFX("click.wav");
                                currentState = GameState::InGame;
                            }
                        });
                    }
                }
            }

            slMenu.Render(renderer);
        }

        if (isFadingOut) {
            fadeAlpha = std::min(fadeAlpha + SCREEN_FADE_SPEED, 255.0f);
            if (fadeAlpha >= 255.0f) {
                if (pendingFadeAction) {
                    pendingFadeAction();
                    pendingFadeAction = nullptr;
                }
                isFadingOut = false;
                isFadingIn = true;
            }
        }
        else if (isFadingIn) {
            fadeAlpha = std::max(fadeAlpha - SCREEN_FADE_SPEED, 0.0f);
            if (fadeAlpha <= 0.0f) {
                isFadingIn = false;
            }
        }

        if (fadeAlpha > 0.0f) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, (Uint8)fadeAlpha);
            SDL_Rect fullscreen = { 0, 0, winW, winH };
            SDL_RenderFillRect(renderer, &fullscreen);
        }

        engine.PresentScreen();
    }

    TextManager::Clean();
    AudioManager::CleanCache();
    engine.Clean();
    return 0;
}