#include "TextureManager.h"
#include <iostream>

std::unordered_map<std::string, SDL_Texture*> TextureManager::textureCache; // static 要再一次

SDL_Texture* TextureManager::LoadTexture(const std::string& fileName, SDL_Renderer* ren) {
	if (textureCache.find(fileName) != textureCache.end()) {
		return textureCache[fileName]; // Cache
	}

	SDL_Texture* tex = IMG_LoadTexture(ren, fileName.c_str());
	if (!tex) {
		std::cerr << "Failed to load image (" << fileName << "): " << IMG_GetError() << std::endl;
	}

	textureCache[fileName] = tex;
	return tex;
}

void TextureManager::Draw(SDL_Texture* tex, SDL_Renderer* ren, int x, int y, float scale) {
	if (!tex) return;
	int originalWidth = 0;
	int originalHeight = 0;

	SDL_QueryTexture(tex, NULL, NULL, &originalWidth, &originalHeight); // Get original width & height

	SDL_Rect destRect = { x, y, static_cast<int>(originalWidth * scale), static_cast<int>(originalHeight * scale) };

	SDL_RenderCopy(ren, tex, NULL, &destRect);
}

void TextureManager::Draw(SDL_Texture* tex, SDL_Renderer* ren, int x, int y, int w, int h, Uint8 alpha) {
	if (!tex) return;

	SDL_SetTextureAlphaMod(tex, alpha);

	SDL_Rect destRect = { x, y, w, h };
	SDL_RenderCopy(ren, tex, NULL, &destRect);

	// 這真的要清
	SDL_SetTextureAlphaMod(tex, 255);
}

void TextureManager::CleanCache() {
	for (auto& pair : textureCache) {
		if (pair.second) {
			SDL_DestroyTexture(pair.second);
		}
	}
	textureCache.clear();
}