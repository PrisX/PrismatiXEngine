#pragma once

#include <string>
#include <vector>

struct SDL_Texture;

namespace PrismatiX::Models {

struct CharacterAnimation {
    std::string type = "fade";
    int totalFrames = 18;
    int currentFrame = 0;
    bool active = false;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    float scale = 1.0f;

    bool IsRunning() const { return active && currentFrame < totalFrames; }
    float Progress() const { return totalFrames > 0 ? static_cast<float>(currentFrame) / totalFrames : 1.0f; }
    void Tick() {
        if (active) {
            ++currentFrame;
            if (currentFrame >= totalFrames) active = false;
        }
    }
    void Start(const std::string& animType, int frames) {
        type = animType;
        totalFrames = frames;
        currentFrame = 0;
        active = true;
    }
};

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

    CharacterAnimation anim;
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

}  // namespace PrismatiX::Models
