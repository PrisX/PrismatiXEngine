#include "VNDialogueSystem.h"

#include <algorithm>

namespace {
std::vector<std::string> SplitUtf8Chars(const std::string& text) {
    std::vector<std::string> chars;
    size_t i = 0;
    while (i < text.length()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = 1;
        if ((c & 0x80) == 0)
            len = 1;
        else if ((c & 0xE0) == 0xC0)
            len = 2;
        else if ((c & 0xF0) == 0xE0)
            len = 3;
        else if ((c & 0xF8) == 0xF0)
            len = 4;

        if (i + len <= text.length()) {
            chars.push_back(text.substr(i, len));
        }
        i += len;
    }
    return chars;
}
}  // namespace

VNDialogueSystem::VNDialogueSystem() {}

void VNDialogueSystem::ParseUTF8(const std::string& text) {
    parsedChars = SplitUtf8Chars(text);
    state.totalChars = static_cast<int>(parsedChars.size());
}

void VNDialogueSystem::SetText(const std::string& speaker, const std::string& text, int speed, SDL_Color txtCol, SDL_Color outCol, const std::string& textEffect) {
    state.fullText = text;
    ParseUTF8(text);
    state.speaker = speaker;
    state.currentDisplayText.clear();
    state.textColor = txtCol;
    state.outlineColor = outCol;
    state.currentIndex = 0;
    state.isFinished = false;
    state.activeEffect = textEffect;
    state.effectProgress = 0.0f;

    textSpeed = speed;
    lastUpdateTime = SDL_GetTicks();
    effectStartTime = SDL_GetTicks();

    if (textSpeed <= 0) {
        ShowAll();
    }
}

void VNDialogueSystem::Update() {
    if (state.isFinished) return;

    Uint32 currentTime = SDL_GetTicks();
    if (state.currentIndex < state.totalChars) {
        if (currentTime - lastUpdateTime >= static_cast<Uint32>(std::max(0, textSpeed))) {
            state.currentDisplayText += parsedChars[state.currentIndex];
            state.currentIndex++;
            lastUpdateTime = currentTime;

            if (state.currentIndex >= state.totalChars) {
                state.isFinished = true;
            }
        }
    }

    state.effectProgress = (float)(currentTime - effectStartTime) / 1000.0f;
}

void VNDialogueSystem::ShowAll() {
    state.currentDisplayText = state.fullText;
    state.currentIndex = state.totalChars;
    state.isFinished = true;
}
