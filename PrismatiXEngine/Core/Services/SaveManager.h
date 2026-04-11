#pragma once

#include <string>
#include <vector>
#include "Core/Models/VNModels.h"

namespace PrismatiX::Services {
class GameState;

class SaveManager {
public:
    explicit SaveManager(GameState& gameState);
    ~SaveManager() = default;

    bool SaveGame(int slot, const std::string& scriptName, int line, const std::string& bgName, const std::string& bgmName, const std::vector<PrismatiX::Models::SavedCharacter>& characters);
    bool LoadGame(int slot, std::string& outScript, int& outLine, std::string& outBg, std::string& outBgm, std::vector<PrismatiX::Models::SavedCharacter>& outCharacters);
    void PeekSaveFile(int slot, bool& outIsEmpty, std::string& outDisplayText);

private:
    GameState& gameState;
};

} // namespace PrismatiX::Services
