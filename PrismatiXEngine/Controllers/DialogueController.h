#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <functional>
#include <map>
#include <sol/sol.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "Managers/SaveManager.h"
#include "Managers/TextManager.h"
#include "Managers/TextureManager.h"
#include "ScriptManager.h"
#include "Utils/EasingUtils.h"
#include "Utils/TransitionUtils.h"

struct ActiveCharacter {
    std::string name;
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

class DialogueController {
private:
    // For S/L
    std::string currentScriptName;
    std::string currentBgName;
    std::string currentBgmName;

    std::vector<std::string> dialogueParsedCharacters;
    std::string dialogueCurrentDisplayText;
    std::string dialogueDisplayedText;
    std::string dialogueCurrentSpeakerName;
    SDL_Color dialogueCurrentTextColor = { 255, 255, 255, 255 };
    SDL_Color dialogueCurrentOutlineColor = { 0, 0, 0, 255 };
    int dialogueCurrentIndex = 0;
    Uint32 dialogueLastTime = 0;
    int dialogueTextSpeed = 50;
    Uint8 dialogueFadeAlpha = 255;
    Uint32 dialogueFadeStartTime = 0;
    static constexpr Uint32 kDialogueFadeDuration = 150;
    std::string dialogueActiveTextEffect;
    Uint32 dialogueEffectStartTime = 0;

    void ParseDialogueUTF8(const std::string& text);
    void SetDialogueText(const std::string& speaker, const std::string& text, int speed, SDL_Color textColor, SDL_Color outlineColor, const std::string& textEffect);
    void UpdateDialogueText();
    void ShowDialogueTextAll();
    bool IsDialogueTextFinished() const;

    using CommandHandler = std::function<void(const VNCommand&)>;
    std::unordered_map<std::string, CommandHandler> commandHandlers;
    void InitializeCommandHandlers();

    void HandleCommandCharImg(const VNCommand& cmd);
    void HandleCommandClearCharImg(const VNCommand& cmd);
    void HandleCommandBg(const VNCommand& cmd);
    void HandleCommandText(const VNCommand& cmd);
    void HandleCommandBgm(const VNCommand& cmd);
    void HandleCommandStopBgm(const VNCommand& cmd);
    void HandleCommandSe(const VNCommand& cmd);
    void HandleCommandSet(const VNCommand& cmd);
    void HandleCommandAdd(const VNCommand& cmd);
    void HandleCommandDel(const VNCommand& cmd);
    void HandleCommandBgmInfo(const VNCommand& cmd);
    void HandleCommandChapter(const VNCommand& cmd);
    void HandleCommandLua(const VNCommand& cmd);
    void HandleCommandTransition(const VNCommand& cmd);
    void HandleCommandLabel(const VNCommand& cmd);
    void HandleCommandJump(const VNCommand& cmd);
    void HandleCommandIf(const VNCommand& cmd);
    void HandleCommandElse(const VNCommand& cmd);
    void HandleCommandEndIf(const VNCommand& cmd);
    void HandleCommandChoice(const VNCommand& cmd);

    SDL_Renderer* renderer;
    SDL_Texture* previousBgTexture = nullptr;
    SDL_Texture* currentBgTexture = nullptr;
    TTF_Font* uiFont;
    bool isShowingBacklog = false;
    int backlogOffset = 0;
    int backlogCooldown = 0;
    float backlogFadeAlpha = 0.0f;
    float bgFadeAlpha = 255.0f;
    std::vector<VNCommand> commands;
    int currentLine;
    int clickCooldown;
    bool isFinished;
    bool hasStarted;
    std::string pendingVoice;
    bool skipNextLog = false;
    bool hasPendingScriptTransition = false;
    std::string pendingScriptTarget;
    std::string pendingTransitionStyle;
    std::string pendingTransitionSpeed;
    std::string pendingTransitionEase;
    bool hasPendingInlineTransition = false;
    std::string pendingInlineTransitionStyle;
    std::string pendingInlineTransitionSpeed;
    std::string pendingInlineTransitionEase;
    std::map<std::string, ActiveCharacter> activeCharacters;
    std::string currentSpeakingChar;
    sol::state* luaState = nullptr;
    BGMInfo infoBanner;
    ChapterBanner chapterBanner;

public:
    DialogueController(TTF_Font* dialogueFont, const std::string& dialogueFontName, int dialogueFontSize, TTF_Font* nameFont, SDL_Renderer* ren, sol::state* lua);

    // For S/L
    std::string GetCurrentScriptName() const;
    int GetCurrentLine() const;
    std::string GetCurrentBgName() const;
    std::string GetCurrentBgmName() const;
    void SetCurrentLine(int line);
    void SetSkipNextLog(bool skip);
    bool PopScriptTransition(std::string& outTargetScript, std::string& outTransitionStyle, std::string& outTransitionSpeed, std::string& outTransitionEase);
    void QueueScriptTransition(const std::string& targetScript, const std::string& transitionStyle = "", const std::string& transitionSpeed = "", const std::string& transitionEase = "");
    bool PopInlineTransition(std::string& outTransitionStyle, std::string& outTransitionSpeed, std::string& outTransitionEase);
    void QueueInlineTransition(const std::string& transitionStyle = "", const std::string& transitionSpeed = "", const std::string& transitionEase = "");
    void ContinueScript();
    std::vector<SavedCharacter> GetSavedCharacters() const;
    void RestoreSavedCharacters(const std::vector<SavedCharacter>& savedChars);
    void RestoreBackground(const std::string& bgName);
    sol::table GetDialogueBoxContext(sol::this_state state, int screenW, int screenH) const;

    SDL_Texture* GetBackground() const;
    void LoadScript(const std::string& scriptName, const std::vector<VNCommand>& newScript);
    bool IsShowingBacklog() const;
    void ToggleBacklog();
    void ScrollBacklog(int direction);
    void RecalculateTargetPositions();
    void ExecuteNextCommands();
    void HandleClick(int mx, int my);
    void Update(int mx, int my);
    void RenderBackground();
    void RenderBacklog(SDL_Renderer* renderer);
    void Render(SDL_Renderer* renderer);
    bool IsScriptFinished() const;
};