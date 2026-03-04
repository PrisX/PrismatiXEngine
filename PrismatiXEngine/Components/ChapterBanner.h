#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <string>
#include "TextManager.h"
#include "TextureManager.h"
#include "Utils/EasingUtils.h"
#include "Utils/TransitionUtils.h"

struct ChapterBanner {
    std::string text;

    enum class State { Idle, SlideIn, Staying, FadeOut } state = State::Idle;

    float currentX = -600.0f;
    float targetX = -1.0f;
    int stayTimer = 0;
    float alpha = 255.0f;

    bool IsActive() const { return state != State::Idle; }

    void Show(const std::string& chapterText) {
        text = chapterText;
        state = State::SlideIn;
        currentX = -600.0f;
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
                stayTimer = 300;
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
        int boxY = 20;

        SDL_Texture* bgTex = TextureManager::LoadTexture("chapterinfo.png", renderer);
        if (bgTex) {
            SDL_Rect destRect = TextureManager::DrawAuto(bgTex, renderer, TextureManager::DisplayMode::TopLeft, a, (int)currentX, boxY, 0.7f);

            if (!text.empty()) {
                SDL_Color textColor = { 255, 240, 180, a };
                SDL_Color outlineColor = { 0, 0, 0, a };
                TextManager::DrawWithOutlineCentered(renderer, font, text, textColor, outlineColor, 1, destRect, a, true);
            }
        }
        else {
            int textW = 0, textH = 0;
            TTF_SizeUTF8(font, text.c_str(), &textW, &textH);
            textW /= TextManager::FONT_OVERSAMPLE;
            textH /= TextManager::FONT_OVERSAMPLE;

            const int padX = 20, padY = 10;
            int boxW = textW + padX * 2;
            int boxH = textH + padY * 2;
            int fbX = (int)currentX;

            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 20, 20, 40, (Uint8)(a * 0.85f));
            SDL_Rect bgRect = { fbX, boxY, boxW, boxH };
            SDL_RenderFillRect(renderer, &bgRect);

            SDL_Color textColor = { 255, 240, 180, a };
            SDL_Color outlineColor = { 0, 0, 0, a };
            TextManager::DrawWithOutline(renderer, font, text, textColor, outlineColor, 1,
                fbX + padX, boxY + padY, 0, a);
        }
    }
};
