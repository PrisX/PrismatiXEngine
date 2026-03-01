#pragma once
#include <vector>
#include <string>
#include <map>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "DialogueBox.h"
#include "ScriptManager.h"
#include "Managers/SaveManager.h"

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
};

class DialogueController {
private:
    // For S/L
    std::string currentScriptName;
    std::string currentBgName;
    std::string currentBgmName;

    DialogueBox* dialogueBox;
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
    std::map<std::string, ActiveCharacter> activeCharacters;

public:
    DialogueController(DialogueBox* box, TTF_Font* font, SDL_Renderer* ren);

    // For S/L
    std::string GetCurrentScriptName() const;
    int GetCurrentLine() const;
    std::string GetCurrentBgName() const;
    std::string GetCurrentBgmName() const;
    void SetCurrentLine(int line);
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
    void RenderBackground(PrismatiXEngine& engine);
    void RenderBacklog(SDL_Renderer* renderer);
    void Render(SDL_Renderer* renderer);
    bool IsScriptFinished() const;
};