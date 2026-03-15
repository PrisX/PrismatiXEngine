#pragma once
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <map>
#include <memory>
#include <sol/sol.hpp>
#include <string>
#include <vector>

#include "Controllers/Internal/DialogueWidgets.h"
#include "Managers/SaveManager.h"
#include "ScriptManager.h"

struct ActiveCharacter {
    std::string name;
    std::string diff;
    int pos;

    float alpha = 0.0f;
    float targetAlpha = 255.0f;
    bool isExiting = false;

    float currentX = 0.0f;
    float targetX = 0.0f;
};

class DialogueController {
private:
    // For S/L
    std::string currentScriptName;
    std::string currentBgName;
    std::string currentBgmName;

    std::unique_ptr<DialogueBox> dialogueBox;
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