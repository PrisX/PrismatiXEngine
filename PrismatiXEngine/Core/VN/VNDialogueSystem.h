#pragma once

#include <SDL2/SDL.h>

#include <sol/sol.hpp>
#include <string>
#include <vector>

struct VNDialogueState {
    std::string speaker;
    std::string fullText;
    std::string currentDisplayText;
    int currentIndex = 0;
    int totalChars = 0;
    bool isFinished = false;

    SDL_Color textColor = { 255, 255, 255, 255 };
    SDL_Color outlineColor = { 0, 0, 0, 255 };
    std::string activeEffect;
    float effectProgress = 0.0f;
};

class VNDialogueSystem {
public:
    VNDialogueSystem();
    ~VNDialogueSystem() = default;

    void SetText(const std::string& speaker, const std::string& text, int speed, SDL_Color textColor, SDL_Color outlineColor, const std::string& textEffect);
    void Update();
    void ShowAll();

    const VNDialogueState& GetState() const { return state; }

private:
    VNDialogueState state;

    std::vector<std::string> parsedChars;
    int textSpeed = 50;
    Uint32 lastUpdateTime = 0;
    Uint32 effectStartTime = 0;

    void ParseUTF8(const std::string& text);
};
