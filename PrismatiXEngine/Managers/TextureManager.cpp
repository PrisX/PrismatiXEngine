#include "TextureManager.h"
#include "ArchiveManager.h"
#include <iostream>
#include <algorithm>

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
	// 如果 return nullptr 可以直接用 if 判斷 owo
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

SDL_Rect TextureManager::DrawAuto(SDL_Texture* tex, SDL_Renderer* ren, DisplayMode mode, Uint8 alpha, int offsetX, int offsetY, float scale) {
	if (!tex || !ren) return SDL_Rect{ 0, 0, 0, 0 };

	int texW = 0;
	int texH = 0;
	if (SDL_QueryTexture(tex, NULL, NULL, &texW, &texH) != 0 || texW <= 0 || texH <= 0) return SDL_Rect{ 0, 0, 0, 0 };

	int renderW = 0;
	int renderH = 0;
	SDL_RenderGetLogicalSize(ren, &renderW, &renderH);
	if (renderW <= 0 || renderH <= 0) {
		if (SDL_GetRendererOutputSize(ren, &renderW, &renderH) != 0 || renderW <= 0 || renderH <= 0) return SDL_Rect{ 0, 0, 0, 0 };
	}

	SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
	SDL_SetTextureAlphaMod(tex, alpha);

	int scaledW = std::max(1, static_cast<int>(texW * scale));
	int scaledH = std::max(1, static_cast<int>(texH * scale));
	SDL_Rect destRect = { 0, 0, scaledW, scaledH };

	switch (mode) {
	case DisplayMode::TopLeft:
		destRect.x = 0;
		destRect.y = 0;
		break;
	case DisplayMode::TopRight:
		destRect.x = renderW - scaledW;
		destRect.y = 0;
		break;
	case DisplayMode::BottomLeft:
		destRect.x = 0;
		destRect.y = renderH - scaledH;
		break;
	case DisplayMode::BottomRight:
		destRect.x = renderW - scaledW;
		destRect.y = renderH - scaledH;
		break;
	case DisplayMode::Top:
		destRect.x = (renderW - scaledW) / 2;
		destRect.y = 0;
		break;
	case DisplayMode::Bottom:
		destRect.x = (renderW - scaledW) / 2;
		destRect.y = renderH - scaledH;
		break;
	case DisplayMode::Left:
		destRect.x = 0;
		destRect.y = (renderH - scaledH) / 2;
		break;
	case DisplayMode::Right:
		destRect.x = renderW - scaledW;
		destRect.y = (renderH - scaledH) / 2;
		break;
	case DisplayMode::Center:
		destRect.x = (renderW - scaledW) / 2;
		destRect.y = (renderH - scaledH) / 2;
		break;
	case DisplayMode::FitWidthBottom:
	{
		float fitScale = static_cast<float>(renderW) / texW * scale;
		destRect.w = std::max(1, static_cast<int>(texW * fitScale));
		destRect.h = std::max(1, static_cast<int>(texH * fitScale));
		destRect.x = 0;
		destRect.y = renderH - destRect.h;
		break;
	}
	case DisplayMode::Fit:
	{
		float fitScale = std::min(static_cast<float>(renderW) / texW, static_cast<float>(renderH) / texH) * scale;
		destRect.w = std::max(1, static_cast<int>(texW * fitScale));
		destRect.h = std::max(1, static_cast<int>(texH * fitScale));
		destRect.x = (renderW - destRect.w) / 2;
		destRect.y = (renderH - destRect.h) / 2;
		break;
	}
	case DisplayMode::Fill:
	{
		float fitScale = std::max(static_cast<float>(renderW) / texW, static_cast<float>(renderH) / texH) * scale;
		destRect.w = std::max(1, static_cast<int>(texW * fitScale));
		destRect.h = std::max(1, static_cast<int>(texH * fitScale));
		destRect.x = (renderW - destRect.w) / 2;
		destRect.y = (renderH - destRect.h) / 2;
		break;
	}
	}

	destRect.x += offsetX;
	destRect.y += offsetY;
	SDL_RenderCopy(ren, tex, NULL, &destRect);
	SDL_SetTextureAlphaMod(tex, 255);
	return destRect;
}

void TextureManager::CleanCache() {
	for (auto& pair : textureCache) {
		if (pair.second) {
			SDL_DestroyTexture(pair.second);
		}
	}
	textureCache.clear();
}