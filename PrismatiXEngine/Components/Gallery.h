#pragma once

#include <SDL2/SDL.h>
#include "TextureManager.h"

class Gallery {


public:
    void Render(SDL_Renderer* renderer) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        SDL_Texture* galleryBgTex = TextureManager::LoadTexture("gallery_bg.png", renderer);
        if (galleryBgTex) {
            TextureManager::DrawAuto(galleryBgTex, renderer, TextureManager::DisplayMode::Fit);
        }
        else {
            int w = 0, h = 0;
            SDL_GetRendererOutputSize(renderer, &w, &h);
            SDL_SetRenderDrawColor(renderer, 15, 15, 25, 255);
            SDL_Rect bg = { 0, 0, w, h };
            SDL_RenderFillRect(renderer, &bg);
        }
    }
};