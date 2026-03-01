#include "SaveManager.h"
#include "VariableManager.h"
#include <fstream>
#include <iostream>

bool SaveManager::SaveGame(int slot, const std::string& scriptName, int line, const std::string& bgName, const std::string& bgmName, const std::vector<SavedCharacter>& characters) {
    std::string fileName = "save_" + std::to_string(slot) + ".sav";
    std::ofstream out(fileName);
    if (!out.is_open()) return false;

    out << "[STATE]\n";
    out << "Script=" << scriptName << "\n";
    out << "Line=" << line << "\n";
    out << "BG=" << bgName << "\n";
    out << "BGM=" << bgmName << "\n";

    out << "[CHARACTERS]\n";
    for (const auto& chara : characters) {
        out << chara.name << "=" << chara.diff << "," << chara.pos << "\n";
    }

    out << "[VARIABLES]\n";
    for (const auto& [key, value] : VariableManager::GetAllFlags()) {
        out << key << "=" << value << "\n";
    }

    out.close();
    std::cout << "Saved game data to" << fileName << "\n";
    return true;
}

bool SaveManager::LoadGame(int slot, std::string& outScript, int& outLine, std::string& outBg, std::string& outBgm, std::vector<SavedCharacter>& outCharacters) {
    std::string fileName = "save_" + std::to_string(slot) + ".sav";
    std::ifstream in(fileName);
    if (!in.is_open()) return false;

    std::string lineStr;
    int currentSection = 0;

    VariableManager::ClearAll();
    outCharacters.clear();

    while (std::getline(in, lineStr)) {
        if (lineStr.empty() || lineStr[0] == ';') continue;

        if (lineStr == "[STATE]") { currentSection = 0; continue; }
        if (lineStr == "[CHARACTERS]") { currentSection = 1; continue; }
        if (lineStr == "[VARIABLES]") { currentSection = 2; continue; }

        size_t eqPos = lineStr.find('=');
        if (eqPos != std::string::npos) {
            std::string key = lineStr.substr(0, eqPos);
            std::string val = lineStr.substr(eqPos + 1);

            if (currentSection == 0) {
                if (key == "Script") outScript = val;
                else if (key == "Line") outLine = std::stoi(val);
                else if (key == "BG") outBg = val;
                else if (key == "BGM") outBgm = val;
            }
            else if (currentSection == 1) {
                size_t commaPos = val.find(',');
                if (commaPos != std::string::npos) {
                    SavedCharacter sc;
                    sc.name = key;
                    sc.diff = val.substr(0, commaPos);
                    sc.pos = std::stoi(val.substr(commaPos + 1));
                    outCharacters.push_back(sc);
                }
            }
            else if (currentSection == 2) {
                VariableManager::Set(key, std::stoi(val));
            }
        }
    }

    in.close();
    std::cout << "Loaded game data from " << fileName << "\n";
    return true;
}