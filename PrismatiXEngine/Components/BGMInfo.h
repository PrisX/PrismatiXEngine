#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include "TextManager.h"
#include "Utils/EasingUtils.h"
#include "Utils/TransitionUtils.h"

struct BGMInfo {
    std::string text;
    bool isMusicNotification = false;

    enum class State { Idle, SlideIn, Staying, FadeOut } state = State::Idle;

    float currentX = -400.0f;
    float targetX = 20.0f;
    int stayTimer = 0;
    float alpha = 255.0f;

    bool IsActive() const { return state != State::Idle; }

    void Show(const std::string& msg, bool isMusic = false) {
        text = msg;
        isMusicNotification = isMusic;
        state = State::SlideIn;
        currentX = -400.0f;
        targetX = 20.0f;
        alpha = 255.0f;
        stayTimer = 0;
    }

    void Update() {
        if (!IsActive()) return;

        const float slideFactor = 0.18f;
        switch (state) {
        case State::SlideIn:
            if (EasingUtils::ExpDecay(currentX, targetX, slideFactor)) {
                state = State::Staying;
                stayTimer = 180;
            }
            break;
        case State::Staying:
            if (--stayTimer <= 0)
                state = State::FadeOut;
            break;
        case State::FadeOut:
            if (TransitionUtils::FadeOut(alpha, 4.0f))
                state = State::Idle;
            break;
        default: break;
        }
    }

    void Render(SDL_Renderer* renderer, TTF_Font* font) const {
        if (!IsActive() || alpha <= 0.0f || !renderer || !font) return;

        Uint8 a = (Uint8)alpha;
        std::string displayText = isMusicNotification ? "\xe2\x99\xaa  " + text : text;

        int textW = 0, textH = 0;
        TTF_SizeUTF8(font, displayText.c_str(), &textW, &textH);
        textW /= TextManager::FONT_OVERSAMPLE;
        textH /= TextManager::FONT_OVERSAMPLE;

        const int padX = 16;
        const int padY = 8;
        int boxX = (int)currentX;
        int boxY = 20;
        int boxW = textW + padX * 2;
        int boxH = textH + padY * 2;

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        Uint8 bgAlpha = (Uint8)(a * 0.82f);
        if (isMusicNotification)
            SDL_SetRenderDrawColor(renderer, 20, 30, 50, bgAlpha);
        else
            SDL_SetRenderDrawColor(renderer, 20, 20, 20, bgAlpha);
        SDL_Rect boxRect = { boxX, boxY, boxW, boxH };
        SDL_RenderFillRect(renderer, &boxRect);

        if (isMusicNotification)
            SDL_SetRenderDrawColor(renderer, 100, 180, 255, a);
        else
            SDL_SetRenderDrawColor(renderer, 255, 220, 80, a);
        SDL_Rect accentRect = { boxX, boxY, 4, boxH };
        SDL_RenderFillRect(renderer, &accentRect);

        SDL_Color textColor = isMusicNotification
            ? SDL_Color{ 180, 220, 255, a }
            : SDL_Color{ 255, 255, 255, a };
        SDL_Color outlineColor = { 0, 0, 0, a };
        TextManager::DrawWithOutline(renderer, font, displayText, textColor, outlineColor, 1,
            boxX + padX, boxY + padY, 0, a);
    }
};
