#pragma once

#include <SDL2/SDL.h>
#include <string>
#include <vector>
#include <TextManager.h>
#include <TextureManager.h>
#include <Utils/TransitionUtils.h>

class DialogueBox {
private:
	std::vector<std::string> parsedCharacters; // 拆字，做進入動畫owob
	std::string currentDisplayText;
	std::string displayedText;     // 已完全顯示的字
	std::string currentSpeakerName;
	SDL_Color currentTextColor;
	SDL_Color currentOutlineColor;

	int currentIndex = 0;
	Uint32 lastTime = 0; // ms

	int textSpeed = 50; // ms per char

	Uint8 fadeAlpha = 255;
	Uint32 fadeStartTime = 0;
	static constexpr Uint32 fadeDuration = 150; // ms (0 -> 255)

	TTF_Font* font;
	TTF_Font* nameFont = nullptr;
	int x, y;

    void ParseUTF8(const std::string& text) {
        parsedCharacters.clear();
        size_t i = 0;
        while (i < text.length()) {
            unsigned char c = text[i];
            size_t len = 1;
            // UTF-8 神奇拆解，反正就是要用記憶體位置 https://stackoverflow.com/questions/45716356/utf-text-in-sdl2
            if ((c & 0x80) == 0) len = 1;         // English / Numbers (1 Byte)
            else if ((c & 0xE0) == 0xC0) len = 2; // Idk tf is this (2 Bytes)
            else if ((c & 0xF0) == 0xE0) len = 3; // Chinese / Japanese (3 Bytes)
            else if ((c & 0xF8) == 0xF0) len = 4; // Emoji (4 Bytes)

            parsedCharacters.push_back(text.substr(i, len));
            i += len;
        }
    }

public:
    // 要把傳進來的先存不然會不見qwq
    DialogueBox(TTF_Font* f, int startX, int startY) {
        font = f;
        x = startX;
        y = startY;
    }
    int GetCurrentIndex() const { return currentIndex; }
    void SetNameFont(TTF_Font* f) { nameFont = f; }
    void SetText(const std::string& name, const std::string& text, int speed, SDL_Color textColor, SDL_Color outlineColor) {
        parsedCharacters.clear();
        ParseUTF8(text);
        currentSpeakerName = name;
        currentDisplayText = "";
        displayedText = "";
        currentTextColor = textColor;
		currentOutlineColor = outlineColor;
        currentIndex = 0;
        textSpeed = speed;
        lastTime = SDL_GetTicks();
        fadeAlpha = 255;
    }

    void Update() {
        Uint32 currentTime = SDL_GetTicks();
        if (currentIndex < parsedCharacters.size()) {
            // 如果經過的時間 > 速度 顯示下一個字
            if (currentTime - lastTime >= (Uint32)textSpeed) {
                displayedText = currentDisplayText;
                currentDisplayText += parsedCharacters[currentIndex];
                currentIndex++;
                lastTime = currentTime;
                fadeAlpha = 0;
                fadeStartTime = currentTime;
            }
        }

        if (fadeAlpha < 255) {
            Uint32 elapsed = SDL_GetTicks() - fadeStartTime;
            fadeAlpha = TransitionUtils::AlphaFromElapsed(elapsed, fadeDuration);
        }
    }

    void Render(SDL_Renderer* renderer) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

 
        SDL_Texture* boxTex = TextureManager::LoadTexture("dialoguebox.png", renderer);

        int boxX = 30, boxY = 560, boxW = 1220, boxH = 140;

        if (boxTex) {
            SDL_Rect boxRect = TextureManager::DrawAuto(boxTex, renderer, TextureManager::DisplayMode::FitWidthBottom, 255);
            boxX = boxRect.x;
            boxY = boxRect.y;
            boxW = boxRect.w;
            boxH = boxRect.h;
        }
        else {
            int renderW = 0;
            int renderH = 0;
            SDL_RenderGetLogicalSize(renderer, &renderW, &renderH);
            if (renderW <= 0 || renderH <= 0) {
                SDL_GetRendererOutputSize(renderer, &renderW, &renderH);
            }
            if (renderW > 0) boxW = renderW;
            if (renderH > 0) boxY = renderH - boxH;
            boxX = 0;

            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
            SDL_Rect uiRect = { boxX, boxY, boxW, boxH };
            SDL_RenderFillRect(renderer, &uiRect);
        }


        if (!currentSpeakerName.empty()) {
            SDL_Texture* namePlateTex = TextureManager::LoadTexture("nameplate.png", renderer);
            SDL_Rect namePlateRect = TextureManager::DrawAuto(namePlateTex, renderer, TextureManager::DisplayMode::BottomLeft, 255, 120, -180, 0.7f, { true, 1, 1, 110 });

            if (nameFont) {
                SDL_Color nameColor = { 255, 255, 255, 255 };
                SDL_Color nameOutline = { 60, 30, 80, 200 };

                if (namePlateRect.w <= 0) {
                    int textW = 0, textH = 0;
                    TTF_SizeUTF8(nameFont, currentSpeakerName.c_str(), &textW, &textH);
                    textW /= TextManager::FONT_OVERSAMPLE;
                    textH /= TextManager::FONT_OVERSAMPLE;
                    const int padX = 14, padY = 6;
                    namePlateRect = { boxX + 20, boxY - textH - padY * 2 - 4, textW + padX * 2, textH + padY * 2 };
                    SDL_SetRenderDrawColor(renderer, 40, 20, 60, 200);
                    SDL_RenderFillRect(renderer, &namePlateRect);
                }

                TextManager::DrawWithOutlineCentered(renderer, nameFont, currentSpeakerName, nameColor, nameOutline, 1, namePlateRect, 255, true);
            }
        }


        if (!currentDisplayText.empty()) {
            SDL_Color textColor = currentTextColor;
            SDL_Color outlineColor = currentOutlineColor;

            Uint32 textBoxWidth = 960;
            int textDrawX = boxX + (boxW - (int)textBoxWidth) / 2;
            int textDrawY = boxY + 20;

            if (fadeAlpha < 255) {
                TextManager::DrawWithOutline(renderer, font, currentDisplayText, textColor, outlineColor, 1, textDrawX, textDrawY, textBoxWidth, fadeAlpha, true);
                if (!displayedText.empty()) {
                    TextManager::DrawWithOutline(renderer, font, displayedText, textColor, outlineColor, 1, textDrawX, textDrawY, textBoxWidth, 255, true);
                }
            }
            else {
                TextManager::DrawWithOutline(renderer, font, currentDisplayText, textColor, outlineColor, 1, textDrawX, textDrawY, textBoxWidth, 255, true);
            }
        }
    }

	// If click
    void ShowAll() {
        if (currentIndex < parsedCharacters.size()) {
            currentDisplayText = "";
            for (const auto& ch : parsedCharacters) {
                currentDisplayText += ch;
            }
            currentIndex = parsedCharacters.size();
        }
        displayedText = currentDisplayText;
        fadeAlpha = 255;
    }
    
    bool IsFinished() const {
        return currentIndex >= parsedCharacters.size();
    }

};