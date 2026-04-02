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

#include <memory>

class RenderSystem;

class UIManager {
public:
    UIManager(std::unique_ptr<RenderSystem>& renderSystemRef);
    ~UIManager() = default;

    void AddTextButton(const std::string& text, TTF_Font* font, SDL_Color idle, SDL_Color hover, const std::string& target, const std::string& transitionStyle = "", const std::string& transitionSpeed = "", const std::string& transitionEase = "");
    void RecalculateLayout(int screenW, int screenH);
    void Clear();
    bool HasButtons() const;
    void UpdateHover(int mouseX, int mouseY);
    bool CheckClick(int mouseX, int mouseY, std::string& outTarget, std::string& outTransitionStyle, std::string& outTransitionSpeed, std::string& outTransitionEase) const;
    std::string GetHoveredText() const;
    void Render(SDL_Renderer* renderer);

private:
    std::unique_ptr<RenderSystem>& renderSystem;
    std::vector<UIButton> buttons;
};