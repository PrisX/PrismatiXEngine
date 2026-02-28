#pragma once
#include <SDL2/SDL.h>
#include <vector>
#include <string>

#include "../Managers/ConfigManager.h"
#include "../Managers/TextureManager.h"
#include "../Managers/AudioManager.h"
#include "../Managers/ScriptManager.h"

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
    void Init(SDL_Renderer* renderer) {
        int logoCount = ConfigManager::GetInt("LogoCount", 0);

        for (int i = 1; i <= logoCount; ++i) {
            SplashLogo logo;
            std::string prefix = "Logo" + std::to_string(i) + "_";

            std::string imgFile = ConfigManager::GetString(prefix + "Image", "");
            if (!imgFile.empty()) logo.texture = TextureManager::LoadTexture(imgFile, renderer);

            std::string colorStr = ConfigManager::GetString(prefix + "BgColor", "0,0,0");
            logo.bgColor = ScriptManager::ParseColor(colorStr, { 0, 0, 0, 255 });

            logo.sfxFile = ConfigManager::GetString(prefix + "SFX", "");

            splashSequence.push_back(logo);
        }

        SplashLogo engineLogo;
        engineLogo.texture = TextureManager::LoadTexture("prismatix_logo.png", renderer);
        engineLogo.bgColor = { 0, 0, 0, 255 };
        engineLogo.sfxFile = "knife.mp3";
        splashSequence.push_back(engineLogo);

        currentLogoIdx = 0;
        splashPhase = SplashPhase::FadeIn;
        splashAlpha = 0.0f;
        splashHoldTimer = 0;
        splashSfxPlayed = false;
        finished = false;
    }

    void Update() {
        if (finished || currentLogoIdx >= splashSequence.size()) return;

        SplashLogo& currentLogo = splashSequence[currentLogoIdx];

        if (splashPhase == SplashPhase::FadeIn) {
            if (!splashSfxPlayed && !currentLogo.sfxFile.empty()) {
                AudioManager::PlaySFX(currentLogo.sfxFile);
                splashSfxPlayed = true;
            }

            splashAlpha += SPLASH_FADE_SPEED;
            if (splashAlpha >= 255.0f) {
                splashAlpha = 255.0f;
                splashPhase = SplashPhase::Hold;
            }
        }
        else if (splashPhase == SplashPhase::Hold) {
            splashHoldTimer++;
            if (splashHoldTimer >= SPLASH_HOLD_TIME) {
                splashPhase = SplashPhase::FadeOut;
            }
        }
        else if (splashPhase == SplashPhase::FadeOut) {
            splashAlpha -= SPLASH_FADE_SPEED;
            if (splashAlpha <= 0.0f) {
                splashAlpha = 0.0f;
                currentLogoIdx++;
                splashPhase = SplashPhase::FadeIn;
                splashHoldTimer = 0;
                splashSfxPlayed = false;

                if (currentLogoIdx >= splashSequence.size()) {
                    finished = true;
                }
            }
        }
    }

    void Render(SDL_Renderer* renderer, int winW, int winH) {
        if (finished || currentLogoIdx >= splashSequence.size()) return;

        SplashLogo& currentLogo = splashSequence[currentLogoIdx];

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, currentLogo.bgColor.r, currentLogo.bgColor.g, currentLogo.bgColor.b, (Uint8)splashAlpha);

        SDL_Rect bgRect = { 0, 0, winW, winH };
        SDL_RenderFillRect(renderer, &bgRect);

        if (currentLogo.texture) {
            SDL_SetTextureBlendMode(currentLogo.texture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureAlphaMod(currentLogo.texture, (Uint8)splashAlpha);

            int lw, lh;
            SDL_QueryTexture(currentLogo.texture, NULL, NULL, &lw, &lh);
            SDL_Rect destRect = { (winW - lw) / 2, (winH - lh) / 2, lw, lh };
            SDL_RenderCopy(renderer, currentLogo.texture, NULL, &destRect);
        }
    }

    void HandleClick() {
        if (finished) return;

        currentLogoIdx++;
        splashPhase = SplashPhase::FadeIn;
        splashAlpha = 0.0f;
        splashHoldTimer = 0;
        splashSfxPlayed = false;

        if (currentLogoIdx >= splashSequence.size()) {
            finished = true;
        }
    }

    bool IsFinished() const { return finished; }
};