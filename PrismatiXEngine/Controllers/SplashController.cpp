#include "SplashController.h"
#include "ConfigManager.h"
#include "TextureManager.h"
#include "AudioManager.h"
#include "ScriptManager.h"


void SplashController::Init(SDL_Renderer* renderer) {
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
    engineLogo.texture = TextureManager::LoadTexture("PrismatiXEngine_Logo.png", renderer);
    engineLogo.bgColor = { 0, 0, 0, 255 };
    engineLogo.sfxFile = "PrismatiXEngine_Logo.wav";
    splashSequence.push_back(engineLogo);

    currentLogoIdx = 0;
    splashPhase = SplashPhase::FadeIn;
    splashAlpha = 0.0f;
    splashHoldTimer = 0;
    splashSfxPlayed = false;
    finished = false;
}

void SplashController::Update() {
    if (finished || currentLogoIdx >= (int)splashSequence.size()) return;

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

            if (currentLogoIdx >= (int)splashSequence.size()) {
                finished = true;
            }
        }
    }
}

void SplashController::Render(SDL_Renderer* renderer, int winW, int winH) {
    if (finished || currentLogoIdx >= (int)splashSequence.size()) return;

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

void SplashController::HandleClick() {
    if (finished) return;

    currentLogoIdx++;
    splashPhase = SplashPhase::FadeIn;
    splashAlpha = 0.0f;
    splashHoldTimer = 0;
    splashSfxPlayed = false;

    if (currentLogoIdx >= (int)splashSequence.size()) {
        finished = true;
    }
}

bool SplashController::IsFinished() const {
    return finished;
}
