#include "UIManager.h"

#include "Core/EngineConfig.h"
#include "Core/Systems/RenderSystem.h"

namespace PrismatiX {
namespace UI {

UIManager::UIManager(Systems::RenderSystem& renSys) : renderSystem(renSys) {}

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
    btn.rect = { 0, 0, w / EngineConfig::kFontOversample, h / EngineConfig::kFontOversample };

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

bool UIManager::HasButtons() const { return !buttons.empty(); }

void UIManager::UpdateHover(int mouseX, int mouseY) {
    for (auto& btn : buttons) {
        btn.isHovered = (mouseX >= btn.rect.x && mouseX <= btn.rect.x + btn.rect.w && mouseY >= btn.rect.y && mouseY <= btn.rect.y + btn.rect.h);
    }
}

bool UIManager::CheckClick(int mouseX, int mouseY, std::string& outTarget, std::string& outTransitionStyle, std::string& outTransitionSpeed, std::string& outTransitionEase) const {
    for (const auto& btn : buttons) {
        if (btn.isHovered) {
            outTarget = btn.target;
            outTransitionStyle = btn.transitionStyle;
            outTransitionSpeed = btn.transitionSpeed;
            outTransitionEase = btn.transitionEase;
            return true;
        }
    }
    return false;
}

std::string UIManager::GetHoveredText() const {
    for (const auto& btn : buttons) {
        if (btn.isHovered) return btn.text;
    }
    return "";
}

void UIManager::Render() {
    for (const auto& btn : buttons) {
        SDL_Color col = btn.isHovered ? btn.hoverColor : btn.idleColor;
        renderSystem.DrawTextWithOutline(btn.font, btn.text, col, { 0, 0, 0, 255 }, 2, btn.rect.x, btn.rect.y);
    }
}

}  // namespace UI
}  // namespace PrismatiX
