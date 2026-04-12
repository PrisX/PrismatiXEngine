#pragma once

#include <string>
#include <vector>

#include "Core/Models/SaveSnapshot.h"

namespace PrismatiX::Services {
class GameState;

class SaveManager {
public:
    explicit SaveManager(GameState& gameState);
    ~SaveManager() = default;

    bool SaveGame(int slot, const PrismatiX::Models::SaveSnapshot& snapshot);
    bool LoadGame(int slot, PrismatiX::Models::SaveSnapshot& outSnapshot);
    void PeekSaveFile(int slot, bool& outIsEmpty, std::string& outDisplayText);

private:
    GameState& gameState;
};

} // namespace PrismatiX::Services
