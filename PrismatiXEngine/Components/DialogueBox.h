#pragma once

#include <SDL2/SDL.h>
#include <string>
#include <vector>
#include <TextManager.h>

class DialogueBox {
private:
	std::vector<std::string> parsedCharacters; // 拆字，做進入動畫owob
	std::string currentDisplayText;

	int currentIndex = 0;
	Uint32 lastTime = 0; // ms

	int textSpeed = 50; // ms per char

	TTF_Font* font;
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

    void SetText(const std::string& text, int speed = 50) {
        ParseUTF8(text);
        currentDisplayText = "";
        currentIndex = 0;
        textSpeed = speed;
        lastTime = SDL_GetTicks();
    }

    void Update() {
        if (currentIndex < parsedCharacters.size()) {
            Uint32 currentTime = SDL_GetTicks();
            // 如果經過的時間 > 速度 顯示下一個字
            if (currentTime - lastTime >= textSpeed) {
                currentDisplayText += parsedCharacters[currentIndex];
                currentIndex++;
                lastTime = currentTime;
            }
        }
    }

    void Render(SDL_Renderer * renderer) {
        if (!currentDisplayText.empty()) {
        SDL_Color textColor = { 255, 255, 255, 255 };
        SDL_Color outlineColor = { 0, 0, 0, 255 };
        Uint32 textBoxWidth = 1000;

        TextManager::DrawWithOutline(renderer, font, currentDisplayText, textColor, outlineColor, 1, x, y, textBoxWidth);
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
    }
    
};