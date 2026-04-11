#pragma once

#include <string>
#include <vector>

struct SDL_Texture;

namespace PrismatiX::Models {

struct ActiveCharacter {
    std::string name;
    std::string diff;
    int pos;

    SDL_Texture* cachedTexture = nullptr;
    std::string cachedDiff;

    float alpha = 0.0f;
    float targetAlpha = 255.0f;
    bool isExiting = false;

    float currentX = 0.0f;
    float targetX = 0.0f;

    std::string animation = "fade";
    int animationDuration = 18;
    int animationFrame = 0;
    bool animationActive = false;
    float renderOffsetX = 0.0f;
    float renderOffsetY = 0.0f;
    float renderScale = 1.0f;
};

struct SavedCharacter {
    std::string name;
    std::string diff;
    int pos;
};

struct BacklogEntry {
    std::string speaker;
    std::string text;
    std::string voice;
    bool isChoice = false;
};

struct NotificationOverlayData {
    enum class Type { Chapter, BGM };
    std::string text;
    Type type = Type::Chapter;
    float currentX = -600.0f;
    float targetX = 20.0f;
    int stayTimer = 0;
    float alpha = 255.0f;
    bool active = false;
};

} // namespace PrismatiX::Models
