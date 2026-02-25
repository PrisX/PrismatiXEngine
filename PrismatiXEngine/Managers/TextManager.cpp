#include "TextManager.h"
#include <iostream>

TTF_Font* TextManager::LoadFont(const std::string& fileName, int fontSize) {
    TTF_Font* font = TTF_OpenFont(fileName.c_str(), fontSize * FONT_OVERSAMPLE);
    if (!font) {
        std::cerr << "Failed to load font (" << fileName << "): " << TTF_GetError() << std::endl;
    }
    return font;
}

void TextManager::Draw(SDL_Renderer* ren, TTF_Font* font, const std::string& text, SDL_Color color, int x, int y) {
    if (!font) return;

    SDL_Surface* surfaceMessage = TTF_RenderUTF8_Blended(font, text.c_str(), color); // UTF8 Blended
    if (!surfaceMessage) return;

    SDL_Texture* message = SDL_CreateTextureFromSurface(ren, surfaceMessage);

	SDL_Rect messageRect = { x, y, surfaceMessage->w / FONT_OVERSAMPLE, surfaceMessage->h / FONT_OVERSAMPLE };

    SDL_RenderCopy(ren, message, NULL, &messageRect);

    // Clean up 
    SDL_FreeSurface(surfaceMessage);
    SDL_DestroyTexture(message);
}

void TextManager::DrawWithOutline(SDL_Renderer* ren, TTF_Font* font, const std::string& text,
    SDL_Color textColor, SDL_Color outlineColor, int outlineSize,
    int x, int y, Uint32 wrapLength, Uint8 alpha) {
    if (!font || text.empty()) return;

    // Outline background
    TTF_SetFontOutline(font, outlineSize * FONT_OVERSAMPLE);
    SDL_Surface* bgSurface = (wrapLength > 0) ?
        TTF_RenderUTF8_Blended_Wrapped(font, text.c_str(), outlineColor, wrapLength * FONT_OVERSAMPLE) :
        TTF_RenderUTF8_Blended(font, text.c_str(), outlineColor);    
    SDL_Texture* bgTexture = SDL_CreateTextureFromSurface(ren, bgSurface);
    SDL_Rect bgRect = { x, y, bgSurface->w / FONT_OVERSAMPLE, bgSurface->h / FONT_OVERSAMPLE };

	// Foreground text
    TTF_SetFontOutline(font, 0); // 這要先關不然會炸
    SDL_Surface* fgSurface = (wrapLength > 0) ?
        TTF_RenderUTF8_Blended_Wrapped(font, text.c_str(), textColor, wrapLength * FONT_OVERSAMPLE) :
        TTF_RenderUTF8_Blended(font, text.c_str(), textColor);
    SDL_Texture* fgTexture = SDL_CreateTextureFromSurface(ren, fgSurface);

	// Align (要根據 outline size 不然整個會跑走)
    SDL_Rect fgRect = { x + outlineSize, y + outlineSize, fgSurface->w / FONT_OVERSAMPLE, fgSurface->h / FONT_OVERSAMPLE };

    SDL_SetTextureAlphaMod(bgTexture, alpha);
    SDL_SetTextureAlphaMod(fgTexture, alpha);
    SDL_RenderCopy(ren, bgTexture, NULL, &bgRect);
    SDL_RenderCopy(ren, fgTexture, NULL, &fgRect);


    // Clean up
    SDL_FreeSurface(bgSurface);
    SDL_DestroyTexture(bgTexture);
    SDL_FreeSurface(fgSurface);
    SDL_DestroyTexture(fgTexture);
}