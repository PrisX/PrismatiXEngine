#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <string>
#include <vector>

#include "Utils/EasingUtils.h"
#include "Utils/TransitionUtils.h"

class Engine;

struct BacklogEntry {
    std::string speaker;
    std::string text;
    std::string voice;
    bool isChoice = false;
};

struct SavedCharacter {
    std::string name;
    std::string diff;
    int pos;
};

struct ActiveCharacter {
    std::string name;
    std::string speakerName;
    std::string diff;
    int pos;

    float alpha = 0.0f;
    float targetAlpha = 255.0f;
    bool isExiting = false;

    float currentX = 0.0f;
    float targetX = 0.0f;

    std::string animation = "fade";
    std::string animationEase;
    std::string animationTrigger = "enter";
    int animationDuration = 18;
    int animationFrame = 0;
    bool animationActive = false;
    float renderOffsetX = 0.0f;
    float renderOffsetY = 0.0f;
    float renderScale = 1.0f;
};

struct ChapterBanner {
    std::string text;
    enum class State { Idle, SlideIn, Staying, FadeOut } state = State::Idle;
    float currentX = -600.0f;
    float targetX = -1.0f;
    int stayTimer = 0;
    float alpha = 255.0f;

    bool IsActive() const { return state != State::Idle; }
    void Show(const std::string& chapterText) {
        text = chapterText;
        state = State::SlideIn;
        currentX = -600.0f;
        alpha = 255.0f;
        stayTimer = 0;
    }
    void Update() {
        if (!IsActive()) return;
        const float slideFactor = 0.18f;
        switch (state) {
            case State::SlideIn:
                if (EasingUtils::ExpDecay(currentX, targetX, slideFactor)) {
                    state = State::Staying;
                    stayTimer = 300;
                }
                break;
            case State::Staying:
                if (--stayTimer <= 0) state = State::FadeOut;
                break;
            case State::FadeOut:
                if (TransitionUtils::FadeOut(alpha, 4.0f)) state = State::Idle;
                break;
            default:
                break;
        }
    }
    void Render(Engine& engine, TTF_Font* font) const;
};

struct BGMInfo {
    std::string text;
    bool isMusicNotification = false;
    enum class State { Idle, SlideIn, Staying, FadeOut } state = State::Idle;
    float currentX = -400.0f;
    float targetX = 20.0f;
    int stayTimer = 0;
    float alpha = 255.0f;

    bool IsActive() const { return state != State::Idle; }
    void Show(const std::string& msg, bool isMusic = false) {
        text = msg;
        isMusicNotification = isMusic;
        state = State::SlideIn;
        currentX = -400.0f;
        targetX = 20.0f;
        alpha = 255.0f;
        stayTimer = 0;
    }
    void Update() {
        if (!IsActive()) return;
        const float slideFactor = 0.18f;
        switch (state) {
            case State::SlideIn:
                if (EasingUtils::ExpDecay(currentX, targetX, slideFactor)) {
                    state = State::Staying;
                    stayTimer = 180;
                }
                break;
            case State::Staying:
                if (--stayTimer <= 0) state = State::FadeOut;
                break;
            case State::FadeOut:
                if (TransitionUtils::FadeOut(alpha, 4.0f)) state = State::Idle;
                break;
            default:
                break;
        }
    }
    void Render(Engine& engine, TTF_Font* font) const;
};
