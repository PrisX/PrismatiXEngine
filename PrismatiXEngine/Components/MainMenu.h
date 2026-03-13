#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <string>
#include <vector>

#include "Managers/TextManager.h"

struct MainMenuButton {
    std::string text;
    std::string action;
    SDL_Rect rect;
    bool isHovered = false;
};

class MainMenu {
private:
    TTF_Font* font;
    std::vector<MainMenuButton> buttons;

public:
    MainMenu(TTF_Font* uiFont, int screenW, int screenH) : font(uiFont) {
        int btnW = 250;
        int btnH = 50;
        int startX = 100;
        int startY = screenH - 250;
        int gap = 60;

        buttons.push_back({ "Start Game", "Start", { startX, startY, btnW, btnH } });
        buttons.push_back({ "Load Game", "Load", { startX, startY + gap, btnW, btnH } });
        buttons.push_back({ "Exit", "Exit", { startX, startY + gap * 2, btnW, btnH } });
    }

    std::string Update(int mouseX, int mouseY, bool isClicked) {
        for (auto& btn : buttons) {
            btn.isHovered = (mouseX >= btn.rect.x && mouseX <= btn.rect.x + btn.rect.w && mouseY >= btn.rect.y && mouseY <= btn.rect.y + btn.rect.h);

            if (isClicked && btn.isHovered) {
                return btn.action;
            }
        }
        return "";
    }

    void Render(SDL_Renderer* renderer) {
        for (const auto& btn : buttons) {
            SDL_Color textCol = { 220, 220, 220, 255 };
            SDL_Color hoverCol = { 255, 215, 0, 255 };

            if (btn.isHovered) {
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 40);
                SDL_RenderFillRect(renderer, &btn.rect);

                TextManager::DrawWithOutline(renderer, font, btn.text, hoverCol, { 0, 0, 0, 255 }, 2, btn.rect.x + 20, btn.rect.y + 10);
            }
            else {
                TextManager::DrawWithOutline(renderer, font, btn.text, textCol, { 0, 0, 0, 255 }, 2, btn.rect.x + 20, btn.rect.y + 10);
            }
        }
    }
};