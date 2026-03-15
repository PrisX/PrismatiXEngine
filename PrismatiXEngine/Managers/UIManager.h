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
    std::string transitionStyle;
    std::string transitionSpeed;
    std::string transitionEase;
};

class UIManager {
private:
    static std::vector<UIButton> buttons;

public:
    static void AddTextButton(const std::string& text, TTF_Font* font, SDL_Color idle, SDL_Color hover, const std::string& target, const std::string& transitionStyle = "", const std::string& transitionSpeed = "", const std::string& transitionEase = "");
    static void RecalculateLayout(int screenW, int screenH);
    static void Clear();
    static bool HasButtons();
    static void UpdateHover(int mouseX, int mouseY);
    static bool CheckClick(int mouseX, int mouseY, std::string& outTarget, std::string& outTransitionStyle, std::string& outTransitionSpeed, std::string& outTransitionEase);
    static std::string GetHoveredText();
    static void Render(SDL_Renderer* renderer);
};