#pragma once

#include <SDL2/SDL.h>

#include <map>
#include <string>
#include <vector>

#include "Core/VN/Models.h"

namespace PrismatiX {
namespace Services {
class ResourceManager;
}
namespace Systems {
class RenderSystem;
}
}  // namespace PrismatiX

namespace PrismatiX {
namespace VN {

class VNStage {
public:
    VNStage(Services::ResourceManager& resMgr, Systems::RenderSystem& renSys);
    ~VNStage() = default;

    void SetBackground(const std::string& bgName, const std::string& transition = "");
    void SetCharacter(const std::string& name, const std::string& diff, int pos, const std::string& transition = "");
    void ClearCharacter(const std::string& name, const std::string& transition = "");

    void Update();
    void Render();

    std::vector<Models::SavedCharacter> GetSavedCharacters() const;
    void RestoreCharacters(const std::vector<Models::SavedCharacter>& savedChars);
    void RestoreBackground(const std::string& bgName);
    std::string GetCurrentBgName() const { return currentBgName; }

private:
    Services::ResourceManager& resourceManager;
    Systems::RenderSystem& renderSystem;

    std::string currentBgName;
    SDL_Texture* currentBgTexture = nullptr;
    SDL_Texture* previousBgTexture = nullptr;
    float bgFadeAlpha = 255.0f;

    std::map<std::string, Models::ActiveCharacter> activeCharacters;
    std::vector<Models::ActiveCharacter*> sortedCharacters;
    bool sortDirty = true;

    void RecalculatePositions();
};

}  // namespace VN
}  // namespace PrismatiX
