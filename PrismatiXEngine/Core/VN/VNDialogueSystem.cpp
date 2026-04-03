#include "VNDialogueSystem.h"

#include <algorithm>

#include "Utils/TransitionUtils.h"

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

void VNDialogueSystem::ParseUTF8(const std::string& text) { parsedChars = SplitUtf8Chars(text); }

void VNDialogueSystem::SetText(const std::string& speaker, const std::string& text, int speed, SDL_Color txtCol, SDL_Color outCol, const std::string& textEffect) {
    fullText = text;
    ParseUTF8(text);
    currentSpeaker = speaker;
    currentDisplayText.clear();
    lastDisplayedText.clear();
    textColor = txtCol;
    outlineColor = outCol;
    currentIndex = 0;
    textSpeed = speed;
    lastUpdateTime = SDL_GetTicks();
    charFadeAlpha = 255;
    activeEffect = textEffect;
    effectStartTime = SDL_GetTicks();

    if (textSpeed <= 0) {
        currentDisplayText = text;
        lastDisplayedText = text;
        currentIndex = static_cast<int>(parsedChars.size());
    }
}

void VNDialogueSystem::Update() {
    Uint32 currentTime = SDL_GetTicks();
    if (currentIndex < static_cast<int>(parsedChars.size())) {
        if (currentTime - lastUpdateTime >= static_cast<Uint32>(std::max(0, textSpeed))) {
            lastDisplayedText = currentDisplayText;
            currentDisplayText += parsedChars[currentIndex];
            currentIndex++;
            lastUpdateTime = currentTime;
            charFadeAlpha = 0;
            charFadeStartTime = currentTime;
        }
    }

    if (charFadeAlpha < 255) {
        Uint32 elapsed = SDL_GetTicks() - charFadeStartTime;
        charFadeAlpha = TransitionUtils::AlphaFromElapsed(elapsed, 150);
    }
}

void VNDialogueSystem::ShowAll() {
    if (currentIndex < static_cast<int>(parsedChars.size())) {
        currentDisplayText = fullText;
        currentIndex = static_cast<int>(parsedChars.size());
    }
    lastDisplayedText = currentDisplayText;
    charFadeAlpha = 255;
}

bool VNDialogueSystem::IsFinished() const { return currentIndex >= static_cast<int>(parsedChars.size()); }

sol::table VNDialogueSystem::GetContext(sol::state_view& lua, int screenW, int screenH) const {
    sol::table ctx = lua.create_table();

    ctx["speaker"] = currentSpeaker;
    ctx["currentText"] = currentDisplayText;
    ctx["displayedText"] = lastDisplayedText;
    ctx["fadeAlpha"] = charFadeAlpha;
    ctx["effect"] = activeEffect;
    ctx["elapsedMs"] = static_cast<int>(SDL_GetTicks() - effectStartTime);

    float progress = parsedChars.empty() ? 1.0f : std::clamp(static_cast<float>(currentIndex) / static_cast<float>(parsedChars.size()), 0.0f, 1.0f);
    ctx["progress"] = progress;
    ctx["screenW"] = screenW;
    ctx["screenH"] = screenH;

    sol::table tCol = lua.create_table();
    tCol["r"] = textColor.r;
    tCol["g"] = textColor.g;
    tCol["b"] = textColor.b;
    tCol["a"] = textColor.a;
    ctx["textColor"] = tCol;

    sol::table oCol = lua.create_table();
    oCol["r"] = outlineColor.r;
    oCol["g"] = outlineColor.g;
    oCol["b"] = outlineColor.b;
    oCol["a"] = outlineColor.a;
    ctx["outlineColor"] = oCol;

    return ctx;
}
