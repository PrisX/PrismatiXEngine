#pragma once

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <iostream>
#include <sol/sol.hpp>
#include <string>
#include <vector>

#include "Managers/TextManager.h"
#include "Managers/TextureManager.h"
#include "Utils/EasingUtils.h"
#include "Utils/TransitionUtils.h"

class DialogueBox {
private:
    std::vector<std::string> parsedCharacters;
    std::string currentDisplayText;
    std::string displayedText;
    std::string currentSpeakerName;
    SDL_Color currentTextColor;
    SDL_Color currentOutlineColor;

    int currentIndex = 0;
    Uint32 lastTime = 0;

    int textSpeed = 50;

    Uint8 fadeAlpha = 255;
    Uint32 fadeStartTime = 0;
    static constexpr Uint32 fadeDuration = 150;

    TTF_Font* font = nullptr;
    TTF_Font* nameFont = nullptr;
    std::string fontName;
    int fontSize = 0;
    std::string activeTextEffect;
    Uint32 effectStartTime = 0;
    sol::state* luaState = nullptr;

    bool TryRenderTextEffect(const std::string& text, int x, int y, Uint32 wrapLength, Uint8 alpha) {
        if (!luaState || activeTextEffect.empty() || text.empty()) {
            return false;
        }

        sol::object effectsObj = (*luaState)["TextEffects"];
        if (!effectsObj.valid() || effectsObj.get_type() != sol::type::table) {
            return false;
        }

        sol::table effects = effectsObj.as<sol::table>();
        sol::protected_function fx = effects[activeTextEffect];
        if (!fx.valid()) {
            return false;
        }

        sol::table ctx = luaState->create_table();
        ctx["text"] = text;
        ctx["speaker"] = currentSpeakerName;
        ctx["x"] = x;
        ctx["y"] = y;
        ctx["wrapLength"] = wrapLength;
        ctx["alpha"] = alpha;
        ctx["fontName"] = fontName;
        ctx["fontSize"] = fontSize;
        ctx["effect"] = activeTextEffect;
        ctx["elapsedMs"] = SDL_GetTicks() - effectStartTime;

        float progress = parsedCharacters.empty() ? 1.0f : static_cast<float>(currentIndex) / static_cast<float>(parsedCharacters.size());
        ctx["progress"] = progress;

        sol::table textColor = luaState->create_table();
        textColor["r"] = currentTextColor.r;
        textColor["g"] = currentTextColor.g;
        textColor["b"] = currentTextColor.b;
        textColor["a"] = currentTextColor.a;
        ctx["textColor"] = textColor;

        sol::table outlineColor = luaState->create_table();
        outlineColor["r"] = currentOutlineColor.r;
        outlineColor["g"] = currentOutlineColor.g;
        outlineColor["b"] = currentOutlineColor.b;
        outlineColor["a"] = currentOutlineColor.a;
        ctx["outlineColor"] = outlineColor;

        sol::protected_function_result result = fx(ctx);
        if (!result.valid()) {
            sol::error err = result;
            std::cerr << "Text effect runtime error (" << activeTextEffect << "): " << err.what() << std::endl;
            activeTextEffect.clear();
            return false;
        }

        if (result.return_count() == 0) {
            return true;
        }

        sol::optional<bool> handled = result;
        if (handled) {
            return *handled;
        }

        return true;
    }

    int MeasureGlyphWidth(const std::string& glyph) const {
        if (!font || glyph.empty()) return 0;

        int w = 0;
        int h = 0;
        TTF_SizeUTF8(font, glyph.c_str(), &w, &h);
        w /= TextManager::FONT_OVERSAMPLE;
        return w;
    }

    int ResolveLineHeight() const {
        if (!font) return 0;

        int lineH = TTF_FontLineSkip(font);
        lineH /= TextManager::FONT_OVERSAMPLE;
        if (lineH <= 0) {
            lineH = fontSize > 0 ? fontSize + 6 : 34;
        }
        return lineH;
    }

    void RenderScatterTypewriterText(SDL_Renderer* renderer, int x, int y, Uint32 wrapLength) const {
        if (!font) return;

        int penX = x;
        int penY = y;
        const int lineHeight = ResolveLineHeight();
        const int maxX = x + static_cast<int>(wrapLength);

        SDL_Color textColor = currentTextColor;
        SDL_Color outlineColor = currentOutlineColor;

        for (int i = 0; i < currentIndex && i < static_cast<int>(parsedCharacters.size()); ++i) {
            const std::string& glyph = parsedCharacters[i];
            if (glyph.empty() || glyph == "\r") {
                continue;
            }

            if (glyph == "\n") {
                penX = x;
                penY += lineHeight;
                continue;
            }

            int glyphW = MeasureGlyphWidth(glyph);
            if (wrapLength > 0 && penX > x && glyphW > 0 && penX + glyphW > maxX) {
                penX = x;
                penY += lineHeight;
            }

            const Uint8 glyphAlpha = (i == currentIndex - 1 && fadeAlpha < 255) ? fadeAlpha : 255;
            TextManager::DrawWithOutline(renderer, font, glyph, textColor, outlineColor, 1, penX, penY, 0, glyphAlpha, true);

            penX += glyphW;
        }
    }

    void ParseUTF8(const std::string& text) {
        parsedCharacters.clear();
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

            parsedCharacters.push_back(text.substr(i, len));
            i += len;
        }
    }

public:
    DialogueBox(TTF_Font* f, const std::string& configuredFontName, int configuredFontSize, sol::state* lua) : font(f), fontName(configuredFontName), fontSize(configuredFontSize), luaState(lua) {}

    int GetCurrentIndex() const { return currentIndex; }

    void SetNameFont(TTF_Font* f) { nameFont = f; }

    void SetText(const std::string& name, const std::string& text, int speed, SDL_Color textColor, SDL_Color outlineColor, const std::string& textEffect = "") {
        parsedCharacters.clear();
        ParseUTF8(text);
        currentSpeakerName = name;
        currentDisplayText = "";
        displayedText = "";
        currentTextColor = textColor;
        currentOutlineColor = outlineColor;
        currentIndex = 0;
        textSpeed = speed;
        lastTime = SDL_GetTicks();
        fadeAlpha = 255;
        activeTextEffect = textEffect;
        effectStartTime = SDL_GetTicks();

        if (textSpeed <= 0) {
            currentDisplayText = text;
            displayedText = text;
            currentIndex = static_cast<int>(parsedCharacters.size());
        }
    }

    void Update() {
        Uint32 currentTime = SDL_GetTicks();
        if (currentIndex < static_cast<int>(parsedCharacters.size())) {
            if (currentTime - lastTime >= static_cast<Uint32>(textSpeed)) {
                displayedText = currentDisplayText;
                currentDisplayText += parsedCharacters[currentIndex];
                currentIndex++;
                lastTime = currentTime;
                fadeAlpha = 0;
                fadeStartTime = currentTime;
            }
        }

        if (fadeAlpha < 255) {
            Uint32 elapsed = SDL_GetTicks() - fadeStartTime;
            fadeAlpha = TransitionUtils::AlphaFromElapsed(elapsed, fadeDuration);
        }
    }

    void Render(SDL_Renderer* renderer) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        SDL_Texture* boxTex = TextureManager::LoadTexture("dialoguebox.png", renderer);

        int boxX = 30;
        int boxY = 560;
        int boxW = 1220;
        int boxH = 140;

        if (boxTex) {
            SDL_Rect boxRect = TextureManager::DrawAuto(boxTex, renderer, TextureManager::DisplayMode::FitWidthBottom, 255);
            boxX = boxRect.x;
            boxY = boxRect.y;
            boxW = boxRect.w;
            boxH = boxRect.h;
        }
        else {
            int renderW = 0;
            int renderH = 0;
            SDL_RenderGetLogicalSize(renderer, &renderW, &renderH);
            if (renderW <= 0 || renderH <= 0) {
                SDL_GetRendererOutputSize(renderer, &renderW, &renderH);
            }
            if (renderW > 0) boxW = renderW;
            if (renderH > 0) boxY = renderH - boxH;
            boxX = 0;

            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 180);
            SDL_Rect uiRect = { boxX, boxY, boxW, boxH };
            SDL_RenderFillRect(renderer, &uiRect);
        }

        if (!currentSpeakerName.empty()) {
            SDL_Texture* namePlateTex = TextureManager::LoadTexture("nameplate.png", renderer);
            SDL_Rect namePlateRect = TextureManager::DrawAuto(namePlateTex, renderer, TextureManager::DisplayMode::BottomLeft, 255, 120, -180, 0.7f, { true, 1, 1, 110 });

            if (nameFont) {
                SDL_Color nameColor = { 255, 255, 255, 255 };
                SDL_Color nameOutline = { 60, 30, 80, 200 };

                if (namePlateRect.w <= 0) {
                    int textW = 0;
                    int textH = 0;
                    TTF_SizeUTF8(nameFont, currentSpeakerName.c_str(), &textW, &textH);
                    textW /= TextManager::FONT_OVERSAMPLE;
                    textH /= TextManager::FONT_OVERSAMPLE;
                    const int padX = 14;
                    const int padY = 6;
                    namePlateRect = { boxX + 20, boxY - textH - padY * 2 - 4, textW + padX * 2, textH + padY * 2 };
                    SDL_SetRenderDrawColor(renderer, 40, 20, 60, 200);
                    SDL_RenderFillRect(renderer, &namePlateRect);
                }

                TextManager::DrawWithOutlineCentered(renderer, nameFont, currentSpeakerName, nameColor, nameOutline, 1, namePlateRect, 255, true);
            }
        }

        if (!currentDisplayText.empty()) {
            SDL_Color textColor = currentTextColor;
            SDL_Color outlineColor = currentOutlineColor;

            Uint32 textBoxWidth = 960;
            int textDrawX = boxX + (boxW - static_cast<int>(textBoxWidth)) / 2;
            int textDrawY = boxY + 20;

            if (activeTextEffect.empty()) {
                RenderScatterTypewriterText(renderer, textDrawX, textDrawY, textBoxWidth);
                return;
            }

            if (fadeAlpha < 255) {
                bool handled = TryRenderTextEffect(currentDisplayText, textDrawX, textDrawY, textBoxWidth, fadeAlpha);
                if (!handled) {
                    TextManager::DrawWithOutline(renderer, font, currentDisplayText, textColor, outlineColor, 1, textDrawX, textDrawY, textBoxWidth, fadeAlpha, true);
                }
                if (!displayedText.empty()) {
                    TextManager::DrawWithOutline(renderer, font, displayedText, textColor, outlineColor, 1, textDrawX, textDrawY, textBoxWidth, 255, true);
                }
            }
            else {
                bool handled = TryRenderTextEffect(currentDisplayText, textDrawX, textDrawY, textBoxWidth, 255);
                if (!handled) {
                    TextManager::DrawWithOutline(renderer, font, currentDisplayText, textColor, outlineColor, 1, textDrawX, textDrawY, textBoxWidth, 255, true);
                }
            }
        }
    }

    void ShowAll() {
        if (currentIndex < static_cast<int>(parsedCharacters.size())) {
            currentDisplayText.clear();
            for (const auto& ch : parsedCharacters) {
                currentDisplayText += ch;
            }
            currentIndex = static_cast<int>(parsedCharacters.size());
        }
        displayedText = currentDisplayText;
        fadeAlpha = 255;
    }

    bool IsFinished() const { return currentIndex >= static_cast<int>(parsedCharacters.size()); }
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

    void Render(SDL_Renderer* renderer, TTF_Font* font) const {
        if (!IsActive() || alpha <= 0.0f || !renderer || !font) return;

        Uint8 a = static_cast<Uint8>(alpha);
        int boxY = 20;

        SDL_Texture* bgTex = TextureManager::LoadTexture("chapterinfo.png", renderer);
        if (bgTex) {
            SDL_Rect destRect = TextureManager::DrawAuto(bgTex, renderer, TextureManager::DisplayMode::TopLeft, a, static_cast<int>(currentX), boxY, 0.7f);

            if (!text.empty()) {
                SDL_Color textColor = { 255, 240, 180, a };
                SDL_Color outlineColor = { 0, 0, 0, a };
                TextManager::DrawWithOutlineCentered(renderer, font, text, textColor, outlineColor, 1, destRect, a, true);
            }
        }
        else {
            int textW = 0;
            int textH = 0;
            TTF_SizeUTF8(font, text.c_str(), &textW, &textH);
            textW /= TextManager::FONT_OVERSAMPLE;
            textH /= TextManager::FONT_OVERSAMPLE;

            const int padX = 20;
            const int padY = 10;
            int boxW = textW + padX * 2;
            int boxH = textH + padY * 2;
            int fbX = static_cast<int>(currentX);

            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 20, 20, 40, static_cast<Uint8>(a * 0.85f));
            SDL_Rect bgRect = { fbX, boxY, boxW, boxH };
            SDL_RenderFillRect(renderer, &bgRect);

            SDL_Color textColor = { 255, 240, 180, a };
            SDL_Color outlineColor = { 0, 0, 0, a };
            TextManager::DrawWithOutline(renderer, font, text, textColor, outlineColor, 1, fbX + padX, boxY + padY, 0, a);
        }
    }
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

    void Render(SDL_Renderer* renderer, TTF_Font* font) const {
        if (!IsActive() || alpha <= 0.0f || !renderer || !font) return;

        Uint8 a = static_cast<Uint8>(alpha);
        std::string displayText = isMusicNotification ? "Music:  " + text : text;

        int textW = 0;
        int textH = 0;
        TTF_SizeUTF8(font, displayText.c_str(), &textW, &textH);
        textW /= TextManager::FONT_OVERSAMPLE;
        textH /= TextManager::FONT_OVERSAMPLE;

        const int padX = 16;
        const int padY = 8;
        int boxX = static_cast<int>(currentX);
        int boxY = 20;
        int boxW = textW + padX * 2;
        int boxH = textH + padY * 2;

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        Uint8 bgAlpha = static_cast<Uint8>(a * 0.82f);
        if (isMusicNotification)
            SDL_SetRenderDrawColor(renderer, 20, 30, 50, bgAlpha);
        else
            SDL_SetRenderDrawColor(renderer, 20, 20, 20, bgAlpha);
        SDL_Rect boxRect = { boxX, boxY, boxW, boxH };
        SDL_RenderFillRect(renderer, &boxRect);

        if (isMusicNotification)
            SDL_SetRenderDrawColor(renderer, 100, 180, 255, a);
        else
            SDL_SetRenderDrawColor(renderer, 255, 220, 80, a);
        SDL_Rect accentRect = { boxX, boxY, 4, boxH };
        SDL_RenderFillRect(renderer, &accentRect);

        SDL_Color textColor = isMusicNotification ? SDL_Color{ 180, 220, 255, a } : SDL_Color{ 255, 255, 255, a };
        SDL_Color outlineColor = { 0, 0, 0, a };
        TextManager::DrawWithOutline(renderer, font, displayText, textColor, outlineColor, 1, boxX + padX, boxY + padY, 0, a);
    }
};
