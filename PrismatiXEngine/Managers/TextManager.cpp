#include "TextManager.h"
#include "ArchiveManager.h"
#include <iostream>

std::unordered_map<std::string, TTF_Font*> TextManager::fontCache;
std::unordered_map<std::string, std::vector<char>> TextManager::fontBuffers;
std::unordered_map<std::string, int> TextManager::fontSizeByKey;
std::unordered_map<TTF_Font*, std::string> TextManager::fontReverseMap;
std::unordered_map<std::string, TTF_Font*> TextManager::outlineFontCache;

TTF_Font* TextManager::LoadFont(const std::string& fileName, int fontSize) {
    std::string cacheKey = fileName + "_" + std::to_string(fontSize);

    if (fontCache.find(cacheKey) != fontCache.end()) {
        return fontCache[cacheKey];
    }

    std::vector<char> buffer = ArchiveManager::ExtractFile(fileName);
    if (buffer.empty()) {
        std::cerr << "Failed to extract font from archive: " << fileName << std::endl;
        return nullptr;
    }

    fontBuffers[cacheKey] = std::move(buffer);

    SDL_RWops* rw = SDL_RWFromMem(fontBuffers[cacheKey].data(), fontBuffers[cacheKey].size());
    TTF_Font* font = TTF_OpenFontRW(rw, 1, fontSize * FONT_OVERSAMPLE);

    if (!font) {
        std::cerr << "Failed to load font from memory (" << fileName << "): " << TTF_GetError() << std::endl;
        return nullptr;
    }

    fontCache[cacheKey] = font;
    fontReverseMap[font] = cacheKey;
    fontSizeByKey[cacheKey] = fontSize;
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

TTF_Font* TextManager::GetOrCreateOutlineFont(TTF_Font* baseFont, int outlineSize) {
    auto keyIt = fontReverseMap.find(baseFont);
    if (keyIt == fontReverseMap.end()) return nullptr;

    const std::string& baseKey = keyIt->second;
    std::string olKey = baseKey + "_ol" + std::to_string(outlineSize);

    auto cached = outlineFontCache.find(olKey);
    if (cached != outlineFontCache.end()) return cached->second;

    auto bufIt = fontBuffers.find(baseKey);
    auto sizeIt = fontSizeByKey.find(baseKey);
    if (bufIt == fontBuffers.end() || sizeIt == fontSizeByKey.end()) return nullptr;

    SDL_RWops* rw = SDL_RWFromMem(bufIt->second.data(), (int)bufIt->second.size());
    TTF_Font* olFont = TTF_OpenFontRW(rw, 1, sizeIt->second * FONT_OVERSAMPLE);
    if (!olFont) return nullptr;

    TTF_SetFontOutline(olFont, outlineSize * FONT_OVERSAMPLE);
    outlineFontCache[olKey] = olFont;
    return olFont;
}

void TextManager::DrawWithOutline(SDL_Renderer* ren, TTF_Font* font, const std::string& text,
    SDL_Color textColor, SDL_Color outlineColor, int outlineSize,
    int x, int y, Uint32 wrapLength, Uint8 alpha) {
    if (!font || text.empty()) return;

	TTF_Font* outlineFont = GetOrCreateOutlineFont(font, outlineSize);

	// Outline background
	SDL_Surface* bgSurface = outlineFont ?
		((wrapLength > 0) ?
			TTF_RenderUTF8_Blended_Wrapped(outlineFont, text.c_str(), outlineColor, wrapLength * FONT_OVERSAMPLE) :
			TTF_RenderUTF8_Blended(outlineFont, text.c_str(), outlineColor))
		: nullptr;
	SDL_Texture* bgTexture = bgSurface ? SDL_CreateTextureFromSurface(ren, bgSurface) : nullptr;
	SDL_Rect bgRect = bgSurface ? SDL_Rect{ x, y, bgSurface->w / FONT_OVERSAMPLE, bgSurface->h / FONT_OVERSAMPLE } : SDL_Rect{};

	// Foreground text
	SDL_Surface* fgSurface = (wrapLength > 0) ?
		TTF_RenderUTF8_Blended_Wrapped(font, text.c_str(), textColor, wrapLength * FONT_OVERSAMPLE) :
		TTF_RenderUTF8_Blended(font, text.c_str(), textColor);
    SDL_Texture* fgTexture = SDL_CreateTextureFromSurface(ren, fgSurface);

	// Align (要根據 outline size 不然整個會跑走)
    SDL_Rect fgRect = { x + outlineSize, y + outlineSize, fgSurface->w / FONT_OVERSAMPLE, fgSurface->h / FONT_OVERSAMPLE };

    if (bgTexture) SDL_SetTextureAlphaMod(bgTexture, alpha);
    SDL_SetTextureAlphaMod(fgTexture, alpha);
    if (bgTexture) SDL_RenderCopy(ren, bgTexture, NULL, &bgRect);
    SDL_RenderCopy(ren, fgTexture, NULL, &fgRect);

    // Clean up
    SDL_FreeSurface(bgSurface);
    SDL_DestroyTexture(bgTexture);
    SDL_FreeSurface(fgSurface);
    SDL_DestroyTexture(fgTexture);
}

void TextManager::Clean() {
    for (auto& pair : outlineFontCache) {
        if (pair.second) TTF_CloseFont(pair.second);
    }
    outlineFontCache.clear();
    for (auto& pair : fontCache) {
        if (pair.second) TTF_CloseFont(pair.second);
    }
    fontCache.clear();
    fontBuffers.clear();
    fontSizeByKey.clear();
    fontReverseMap.clear();
}