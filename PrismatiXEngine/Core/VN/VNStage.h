#pragma once

#include <SDL2/SDL.h>

#include <map>
#include <string>
#include <vector>

#include "Core/Services/ResourceManager.h"
#include "Core/Systems/RenderSystem.h"
#include "Core/VN/Models.h"

class VNStage {
public:
    VNStage(ResourceManager& resMgr, RenderSystem& renSys);
    ~VNStage() = default;

    void SetBackground(const std::string& bgName, const std::string& transition = "");
    void SetCharacter(const std::string& name, const std::string& diff, int pos, const std::string& transition = "");
    void ClearCharacter(const std::string& name, const std::string& transition = "");

    void Update();
    void Render();

    std::vector<SavedCharacter> GetSavedCharacters() const;
    void RestoreCharacters(const std::vector<SavedCharacter>& savedChars);
    void RestoreBackground(const std::string& bgName);
    std::string GetCurrentBgName() const { return currentBgName; }

private:
    ResourceManager& resourceManager;
    RenderSystem& renderSystem;

    std::string currentBgName;
    SDL_Texture* currentBgTexture = nullptr;
    SDL_Texture* previousBgTexture = nullptr;
    float bgFadeAlpha = 255.0f;

    std::map<std::string, ActiveCharacter> activeCharacters;
    std::vector<ActiveCharacter*> sortedCharacters;
    bool sortDirty = true;

    void RecalculatePositions();
};
