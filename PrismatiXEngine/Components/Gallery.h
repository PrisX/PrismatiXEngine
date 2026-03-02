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
    }
};