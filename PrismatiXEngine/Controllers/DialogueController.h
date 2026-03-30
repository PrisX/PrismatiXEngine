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

class PrismatiXEngine;

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

    void Render(PrismatiXEngine& engine, TTF_Font* font) const;
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

    void Render(PrismatiXEngine& engine, TTF_Font* font) const;
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

    PrismatiXEngine& engine;
    SDL_Renderer* renderer;  // Keep this cached or just use engine.GetRenderer() owob
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
    BGMInfo infoBanner;
    ChapterBanner chapterBanner;

public:
    DialogueController(PrismatiXEngine& engine, TTF_Font* dialogueFont, const std::string& dialogueFontName, int dialogueFontSize, TTF_Font* nameFont);

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