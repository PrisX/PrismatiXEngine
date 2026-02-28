#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <vector>
#include "TextManager.h"

struct UIButton {
    std::string text;
    TTF_Font* font;
    SDL_Rect rect;
    SDL_Color idleColor;
    SDL_Color hoverColor;
    bool isHovered;
    std::string target; 
};

class UIManager {
private:
    static inline std::vector<UIButton> buttons;

public:
    static void AddTextButton(const std::string& text, TTF_Font* font, int x, int y, SDL_Color idle, SDL_Color hover, const std::string& target) {
        UIButton btn;
        btn.text = text;
        btn.font = font;
        btn.idleColor = idle;
        btn.hoverColor = hover;
        btn.target = target;
        btn.isHovered = false;

        int w = 0, h = 0;
        if (font && !text.empty()) {
            TTF_SizeUTF8(font, text.c_str(), &w, &h);
        }
        btn.rect = { x, y, w / TextManager::FONT_OVERSAMPLE, h / TextManager::FONT_OVERSAMPLE };

        buttons.push_back(btn);
    }

    static void Clear() { buttons.clear(); }
    static bool HasButtons() { return !buttons.empty(); }

    static void UpdateHover(int mouseX, int mouseY) {
        for (auto& btn : buttons) {
            bool inX = (mouseX >= btn.rect.x && mouseX <= btn.rect.x + btn.rect.w);
            bool inY = (mouseY >= btn.rect.y && mouseY <= btn.rect.y + btn.rect.h);
            btn.isHovered = (inX && inY);
        }
    }

    static std::string CheckClick(int mouseX, int mouseY) {
        for (auto& btn : buttons) {
            if (btn.isHovered) {
                return btn.target; 
            }
        }
        return "";
    }


    static void Render(SDL_Renderer* renderer) {
        for (const auto& btn : buttons) {
            SDL_Color colorToDraw = btn.isHovered ? btn.hoverColor : btn.idleColor;
            TextManager::DrawWithOutline(renderer, btn.font, btn.text, colorToDraw, { 0, 0, 0, 255 }, 2, btn.rect.x, btn.rect.y);
        }
    }
};