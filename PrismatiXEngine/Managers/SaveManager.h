#pragma once
#include <string>
#include <vector>

struct SavedCharacter {
    std::string name;
    std::string diff;
    int pos;
};

class VariableManager;
class BacklogManager;

class SaveManager {
public:
    SaveManager(VariableManager& varMgr, BacklogManager& backlogMgr);
    ~SaveManager() = default;

    bool SaveGame(int slot, const std::string& scriptName, int line, const std::string& bgName, const std::string& bgmName, const std::vector<SavedCharacter>& characters);
    bool LoadGame(int slot, std::string& outScript, int& outLine, std::string& outBg, std::string& outBgm, std::vector<SavedCharacter>& outCharacters);
    void PeekSaveFile(int slot, bool& outIsEmpty, std::string& outDisplayText);

private:
    VariableManager& variableManager;
    BacklogManager& backlogManager;
};