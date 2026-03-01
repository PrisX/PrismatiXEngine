#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <vector>

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
    static std::vector<UIButton> buttons;

public:
    static void AddTextButton(const std::string& text, TTF_Font* font, SDL_Color idle, SDL_Color hover, const std::string& target);
    static void RecalculateLayout(int screenW, int screenH);
    static void Clear();
    static bool HasButtons();
    static void UpdateHover(int mouseX, int mouseY);
    static std::string CheckClick(int mouseX, int mouseY);
    static std::string GetHoveredText();
    static void Render(SDL_Renderer* renderer);
};