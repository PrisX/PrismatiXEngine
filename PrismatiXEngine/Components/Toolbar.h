#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <string>

#include "TextManager.h"

class Toolbar {
private:
    float currentY;
    float targetY;
    float hiddenY;
    float visibleY;
    bool isPinned;

    TTF_Font* font;

    SDL_Rect btnSave;
    SDL_Rect btnLoad;
    SDL_Rect btnPin;

    bool hoverSave = false;
    bool hoverLoad = false;
    bool hoverPin = false;

    void MeasureButton(const std::string& text, SDL_Rect& rect) {
        int w = 0, h = 0;
        if (font) TTF_SizeUTF8(font, text.c_str(), &w, &h);
        int os = TextManager::FONT_OVERSAMPLE;
        rect.w = w / os;
        rect.h = h / os;
    }

public:
    Toolbar(TTF_Font* uiFont, int screenH) {
        font = uiFont;
        hiddenY = (float)screenH;
        visibleY = (float)(screenH - 50);
        currentY = hiddenY;
        targetY = hiddenY;
        isPinned = false;

        btnSave = { 950, 0, 0, 0 };
        btnLoad = { 1050, 0, 0, 0 };
        btnPin = { 1150, 0, 0, 0 };

        MeasureButton("Save", btnSave);
        MeasureButton("Load", btnLoad);
        MeasureButton("Pin", btnPin);
    }

    bool IsMouseOver(int mouseY) const { return mouseY >= currentY; }

    std::string Update(int mouseX, int mouseY, bool isClicked) {
        bool inTriggerArea = (mouseY > visibleY - 20);

        if (isPinned || inTriggerArea) {
            targetY = visibleY;
        }
        else {
            targetY = hiddenY;
        }

        currentY += (targetY - currentY) * 0.15f;

        int textOffsetY = (50 - btnSave.h) / 2;
        btnSave.y = (int)currentY + textOffsetY;
        btnLoad.y = (int)currentY + textOffsetY;
        btnPin.y = (int)currentY + textOffsetY;

        if (currentY > visibleY + 5.0f) {
            hoverSave = hoverLoad = hoverPin = false;
            return "";
        }

        hoverSave = (mouseX >= btnSave.x && mouseX <= btnSave.x + btnSave.w && mouseY >= btnSave.y && mouseY <= btnSave.y + btnSave.h);
        hoverLoad = (mouseX >= btnLoad.x && mouseX <= btnLoad.x + btnLoad.w && mouseY >= btnLoad.y && mouseY <= btnLoad.y + btnLoad.h);
        hoverPin = (mouseX >= btnPin.x && mouseX <= btnPin.x + btnPin.w && mouseY >= btnPin.y && mouseY <= btnPin.y + btnPin.h);

        if (isClicked) {
            if (hoverSave) return "OpenSave";
            if (hoverLoad) return "OpenLoad";
            if (hoverPin) {
                isPinned = !isPinned;
                return "TogglePin";
            }
        }
        return "";
    }

    void Render(SDL_Renderer* renderer) {
        if (currentY >= hiddenY - 1.0f) return;

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
        SDL_Rect bgRect = { 0, (int)currentY, 1280, 50 };
        SDL_RenderFillRect(renderer, &bgRect);

        SDL_Color idleCol = { 200, 200, 200, 255 };
        SDL_Color hoverCol = { 255, 215, 0, 255 };
        SDL_Color pinnedCol = { 100, 255, 100, 255 };

        TextManager::Draw(renderer, font, "Save", hoverSave ? hoverCol : idleCol, btnSave.x, btnSave.y);
        TextManager::Draw(renderer, font, "Load", hoverLoad ? hoverCol : idleCol, btnLoad.x, btnLoad.y);

        SDL_Color pinColor = idleCol;
        if (isPinned)
            pinColor = pinnedCol;
        else if (hoverPin)
            pinColor = hoverCol;

        TextManager::Draw(renderer, font, isPinned ? "Unpin" : "Pin", pinColor, btnPin.x, btnPin.y);
    }
};