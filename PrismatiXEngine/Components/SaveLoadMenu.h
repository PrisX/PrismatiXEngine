#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include <vector>
#include <fstream>
#include "Managers/TextManager.h"
#include "Managers/SaveManager.h"

enum class SLMode { Save, Load };

struct SaveSlot {
    int id;
    SDL_Rect rect;
    bool isEmpty;
    std::string displayText;
    bool isHovered;
};

class SaveLoadMenu {
private:
    TTF_Font* font;
    SLMode mode;
    std::vector<SaveSlot> slots;
    SDL_Rect btnClose;
    bool hoverClose = false;

    void PeekSaveFile(SaveSlot& slot) {
        std::string fileName = "save_" + std::to_string(slot.id) + ".sav";
        std::ifstream in(fileName);
        if (!in.is_open()) {
            slot.isEmpty = true;
            slot.displayText = "NO DATA";
            return;
        }

        slot.isEmpty = false;
        std::string lineStr, scriptName, lineNum;
        while (std::getline(in, lineStr)) {
            if (lineStr == "[VARIABLES]") break; 
            size_t eqPos = lineStr.find('=');
            if (eqPos != std::string::npos) {
                std::string key = lineStr.substr(0, eqPos);
                std::string val = lineStr.substr(eqPos + 1);
                if (key == "Script") scriptName = val;
                if (key == "Line") lineNum = val;
            }
        }
        in.close();
        slot.displayText = scriptName + " (Line: " + lineNum + ")";
    }

public:
    SaveLoadMenu(TTF_Font* uiFont) : font(uiFont), mode(SLMode::Save) {
        int startX = 200, startY = 150;
        int slotW = 260, slotH = 120;
        int gapX = 40, gapY = 40;

        for (int i = 0; i < 9; ++i) {
            SaveSlot slot;
            slot.id = i + 1;
            slot.rect = { startX + (i % 3) * (slotW + gapX),
                          startY + (i / 3) * (slotH + gapY),
                          slotW, slotH };
            slot.isHovered = false;
            slots.push_back(slot);
        }

        btnClose = { 1100, 50, 100, 50 };
    }

    void Open(SLMode newMode) {
        mode = newMode;
        for (auto& slot : slots) {
            PeekSaveFile(slot);
        }
    }

    int Update(int mouseX, int mouseY, bool isClicked) {
        hoverClose = (mouseX >= btnClose.x && mouseX <= btnClose.x + btnClose.w &&
            mouseY >= btnClose.y && mouseY <= btnClose.y + btnClose.h);

        if (isClicked && hoverClose) return -1;

        for (auto& slot : slots) {
            slot.isHovered = (mouseX >= slot.rect.x && mouseX <= slot.rect.x + slot.rect.w &&
                mouseY >= slot.rect.y && mouseY <= slot.rect.y + slot.rect.h);

            if (isClicked && slot.isHovered) {
                if (mode == SLMode::Load && slot.isEmpty) continue;
                return slot.id;
            }
        }
        return 0;
    }

    void Render(SDL_Renderer* renderer) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 230);
        SDL_Rect bgRect = { 0, 0, 1280, 720 };
        SDL_RenderFillRect(renderer, &bgRect);

        SDL_Color textCol = { 255, 255, 255, 255 };
        SDL_Color hoverCol = { 255, 215, 0, 255 };
        std::string title = (mode == SLMode::Save) ? "--- 存檔 SAVE ---" : "--- 讀檔 LOAD ---";
        TextManager::DrawWithOutline(renderer, font, title, textCol, { 0,0,0,255 }, 2, 50, 40);
        TextManager::Draw(renderer, font, "Return", hoverClose ? hoverCol : textCol, btnClose.x, btnClose.y);

        for (const auto& slot : slots) {
            if (slot.isHovered) SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
            else SDL_SetRenderDrawColor(renderer, 40, 40, 40, 255);
            SDL_RenderFillRect(renderer, &slot.rect);

            SDL_SetRenderDrawColor(renderer, 200, 200, 200, 255);
            SDL_RenderDrawRect(renderer, &slot.rect);

            TextManager::Draw(renderer, font, "No." + std::to_string(slot.id), { 150, 150, 150, 255 }, slot.rect.x + 10, slot.rect.y + 10);

            SDL_Color dataCol = slot.isEmpty ? SDL_Color{ 100, 100, 100, 255 } : textCol;
            TextManager::Draw(renderer, font, slot.displayText, dataCol, slot.rect.x + 20, slot.rect.y + 50);
        }
    }
};