#include "UIManager.h"

#include "TextManager.h"

std::vector<UIButton> UIManager::buttons;

void UIManager::AddTextButton(const std::string& text, TTF_Font* font, SDL_Color idle, SDL_Color hover, const std::string& target, const std::string& transitionStyle, const std::string& transitionSpeed, const std::string& transitionEase) {
    UIButton btn;
    btn.text = text;
    btn.font = font;
    btn.idleColor = idle;
    btn.hoverColor = hover;
    btn.target = target;
    btn.transitionStyle = transitionStyle;
    btn.transitionSpeed = transitionSpeed;
    btn.transitionEase = transitionEase;
    btn.isHovered = false;

    int w = 0, h = 0;
    if (font && !text.empty()) {
        TTF_SizeUTF8(font, text.c_str(), &w, &h);
    }
    btn.rect = { 0, 0, w / TextManager::FONT_OVERSAMPLE, h / TextManager::FONT_OVERSAMPLE };

    buttons.push_back(btn);
}

void UIManager::RecalculateLayout(int screenW, int screenH) {
    if (buttons.empty()) return;

    const int gap = 20;
    int totalH = 0;
    for (const auto& btn : buttons) totalH += btn.rect.h;
    totalH += gap * ((int)buttons.size() - 1);

    int curY = (screenH - totalH) / 2;
    for (auto& btn : buttons) {
        btn.rect.x = (screenW - btn.rect.w) / 2;
        btn.rect.y = curY;
        curY += btn.rect.h + gap;
    }
}

void UIManager::Clear() { buttons.clear(); }

bool UIManager::HasButtons() { return !buttons.empty(); }

void UIManager::UpdateHover(int mouseX, int mouseY) {
    for (auto& btn : buttons) {
        bool inX = (mouseX >= btn.rect.x && mouseX <= btn.rect.x + btn.rect.w);
        bool inY = (mouseY >= btn.rect.y && mouseY <= btn.rect.y + btn.rect.h);
        btn.isHovered = (inX && inY);
    }
}

bool UIManager::CheckClick(int mouseX, int mouseY, std::string& outTarget, std::string& outTransitionStyle, std::string& outTransitionSpeed, std::string& outTransitionEase) {
    for (auto& btn : buttons) {
        if (btn.isHovered) {
            outTarget = btn.target;
            outTransitionStyle = btn.transitionStyle;
            outTransitionSpeed = btn.transitionSpeed;
            outTransitionEase = btn.transitionEase;
            return true;
        }
    }
    outTarget.clear();
    outTransitionStyle.clear();
    outTransitionSpeed.clear();
    outTransitionEase.clear();
    return false;
}

std::string UIManager::GetHoveredText() {
    for (const auto& btn : buttons) {
        if (btn.isHovered) {
            return btn.text;
        }
    }
    return "";
}

void UIManager::Render(SDL_Renderer* renderer) {
    for (const auto& btn : buttons) {
        SDL_Color colorToDraw = btn.isHovered ? btn.hoverColor : btn.idleColor;
        TextManager::DrawWithOutline(renderer, btn.font, btn.text, colorToDraw, { 0, 0, 0, 255 }, 2, btn.rect.x, btn.rect.y);
    }
}
