#pragma once

#include <SDL2/SDL.h>

#include <sol/sol.hpp>
#include <string>
#include <vector>

class VNDialogueSystem {
public:
    VNDialogueSystem();
    ~VNDialogueSystem() = default;

    void SetText(const std::string& speaker, const std::string& text, int speed, SDL_Color textColor, SDL_Color outlineColor, const std::string& textEffect);
    void Update();
    void ShowAll();
    bool IsFinished() const;

    // For Lua UI access
    sol::table GetContext(sol::state_view& lua, int screenW, int screenH) const;

    // Getters
    const std::string& GetSpeaker() const { return currentSpeaker; }
    const std::string& GetDisplayText() const { return currentDisplayText; }

private:
    std::string currentSpeaker;
    std::string fullText;
    std::vector<std::string> parsedChars;
    std::string currentDisplayText;
    std::string lastDisplayedText;

    SDL_Color textColor = { 255, 255, 255, 255 };
    SDL_Color outlineColor = { 0, 0, 0, 255 };

    int currentIndex = 0;
    int textSpeed = 50;
    Uint32 lastUpdateTime = 0;

    Uint8 charFadeAlpha = 255;
    Uint32 charFadeStartTime = 0;
    static constexpr Uint32 kCharFadeDuration = 150;

    std::string activeEffect;
    Uint32 effectStartTime = 0;

    void ParseUTF8(const std::string& text);
};
