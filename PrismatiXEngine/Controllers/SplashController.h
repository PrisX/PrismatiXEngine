#pragma once
#include <SDL2/SDL.h>
#include <vector>
#include <string>

struct SplashLogo {
    SDL_Texture* texture = nullptr;
    SDL_Color bgColor = { 0, 0, 0, 255 };
    std::string sfxFile = "";
};

enum class SplashPhase {
    FadeIn,
    Hold,
    FadeOut
};

class SplashController {
private:
    std::vector<SplashLogo> splashSequence;
    int currentLogoIdx = 0;
    SplashPhase splashPhase = SplashPhase::FadeIn;
    float splashAlpha = 0.0f;
    int splashHoldTimer = 0;
    bool splashSfxPlayed = false;
    bool finished = false;

    const float SPLASH_FADE_SPEED = 3.0f;
    const int SPLASH_HOLD_TIME = 90;

public:
    void Init(SDL_Renderer* renderer);
    void Update();
    void Render(SDL_Renderer* renderer, int winW, int winH);
    void HandleClick();
    bool IsFinished() const;
};