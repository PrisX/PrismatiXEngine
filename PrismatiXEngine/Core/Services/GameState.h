#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "Core/VN/Models.h"

class GameState {
public:
    GameState();
    ~GameState() = default;

    // Variables (Flags)
    void SetFlag(const std::string& name, int value);
    void AddFlag(const std::string& name, int value);
    int GetFlag(const std::string& name);
    const std::unordered_map<std::string, int>& GetAllFlags() const;
    bool CheckFlag(const std::string& name, const std::string& op, int compareVal);
    void ClearFlags();

    // Backlog (History)
    void AddLog(const std::string& speaker, const std::string& text, const std::string& voice = "");
    void AddChoiceLog(const std::string& text);
    void ClearLogs();
    const std::vector<BacklogEntry>& GetLogs() const;

    // Persistence (Save/Load)
    bool SaveGame(int slot, const std::string& scriptName, int line, const std::string& bgName, const std::string& bgmName, const std::vector<SavedCharacter>& characters);
    bool LoadGame(int slot, std::string& outScript, int& outLine, std::string& outBg, std::string& outBgm, std::vector<SavedCharacter>& outCharacters);
    void PeekSaveFile(int slot, bool& outIsEmpty, std::string& outDisplayText);

private:
    std::unordered_map<std::string, int> flags;
    std::vector<BacklogEntry> logs;
};
