#pragma once
#include <string>
#include <vector>

struct SavedCharacter {
    std::string name;
    std::string diff;
    int pos;
};

class SaveManager {
public:
    static bool SaveGame(int slot, const std::string& scriptName, int line, const std::string& bgName, const std::string& bgmName, const std::vector<SavedCharacter>& characters);
    static bool LoadGame(int slot, std::string& outScript, int& outLine, std::string& outBg, std::string& outBgm, std::vector<SavedCharacter>& outCharacters);
};