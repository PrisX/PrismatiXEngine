#include "TextureManager.h"
#include "ArchiveManager.h"
#include <iostream>

std::unordered_map<std::string, SDL_Texture*> TextureManager::textureCache; // static 要再一次

SDL_Texture* TextureManager::LoadTexture(const std::string& fileName, SDL_Renderer* ren) {
	if (textureCache.find(fileName) != textureCache.end()) {
		return textureCache[fileName]; // Cache
	}

	std::vector<char> buffer = ArchiveManager::ExtractFile(fileName);
	if (buffer.empty()) return nullptr;

	SDL_RWops* rw = SDL_RWFromMem(buffer.data(), buffer.size());
	if (!rw) return nullptr;

	SDL_Texture* tex = IMG_LoadTexture_RW(ren, rw, 1);

	if (!tex) {
		std::cerr << "Failed to load image (" << fileName << "): " << IMG_GetError() << std::endl;
		return nullptr;
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

	SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
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