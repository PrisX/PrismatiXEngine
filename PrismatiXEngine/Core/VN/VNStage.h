#pragma once

#include <SDL2/SDL.h>

#include <map>
#include <string>
#include <vector>

#include "Core/Models/VNModels.h"
#include "Utils/TransitionUtils.h"

namespace PrismatiX::Services {
class ResourceManager;
}
namespace PrismatiX::Systems {
class RenderSystem;
}

namespace PrismatiX::VN {

class VNStage {
public:
    VNStage(PrismatiX::Services::ResourceManager& resMgr, PrismatiX::Systems::RenderSystem& renSys);
    ~VNStage() = default;

    void SetBackground(const std::string& bgName, const std::string& transition = "");
    void SetCharacter(const std::string& name, const std::string& diff, int pos, const std::string& transition = "");
    void ClearCharacter(const std::string& name, const std::string& transition = "");

    void Update();
    void Render();

    std::vector<PrismatiX::Models::SavedCharacter> GetSavedCharacters() const;
    void RestoreCharacters(const std::vector<PrismatiX::Models::SavedCharacter>& savedChars);
    void RestoreBackground(const std::string& bgName);
    std::string GetCurrentBgName() const { return currentBgName; }

private:
    PrismatiX::Services::ResourceManager& resourceManager;
    PrismatiX::Systems::RenderSystem& renderSystem;

    std::string currentBgName;
    SDL_Texture* currentBgTexture = nullptr;
    SDL_Texture* previousBgTexture = nullptr;
    float bgFadeAlpha = 255.0f;
    PrismatiX::Utils::Transition bgTransition;

    std::map<std::string, PrismatiX::Models::ActiveCharacter> activeCharacters;
    std::vector<PrismatiX::Models::ActiveCharacter*> sortedCharacters;
    bool sortDirty = true;

    void RecalculatePositions();
};

} // namespace PrismatiX::VN
