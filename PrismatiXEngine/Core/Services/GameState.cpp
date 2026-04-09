#include "GameState.h"

#include <Core/EngineConfig.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace PrismatiX {
namespace Services {

GameState::GameState() {}

// Variables (Flags)
void GameState::SetFlag(const std::string& name, int value) { flags[name] = value; }

void GameState::AddFlag(const std::string& name, int value) { flags[name] += value; }

int GameState::GetFlag(const std::string& name) {
    auto it = flags.find(name);
    if (it != flags.end()) return it->second;
    return 0;
}

const std::unordered_map<std::string, int>& GameState::GetAllFlags() const { return flags; }

bool GameState::CheckFlag(const std::string& name, const std::string& op, int compareVal) {
    int currentVal = GetFlag(name);
    if (op == "==") return currentVal == compareVal;
    if (op == "!=") return currentVal != compareVal;
    if (op == ">") return currentVal > compareVal;
    if (op == "<") return currentVal < compareVal;
    if (op == ">=") return currentVal >= compareVal;
    if (op == "<=") return currentVal <= compareVal;
    return false;
}

void GameState::ClearFlags() { flags.clear(); }

// Backlog (History)
void GameState::AddLog(const std::string& speaker, const std::string& text, const std::string& voice) {
    if (text.empty()) return;
    logs.push_back({ speaker, text, voice, false });
}

void GameState::AddChoiceLog(const std::string& text) { logs.push_back({ "", text, "", true }); }

void GameState::ClearLogs() { logs.clear(); }

const std::vector<Models::BacklogEntry>& GameState::GetLogs() const { return logs; }

// Persistence (Save/Load)
bool GameState::SaveGame(int slot, const std::string& scriptName, int line, const std::string& bgName, const std::string& bgmName, const std::vector<Models::SavedCharacter>& characters) {
    fs::create_directories(EngineConfig::kSaveDirectory);
    std::string fileName = EngineConfig::kSaveDirectory + "/" + EngineConfig::kSaveFilePrefix + std::to_string(slot) + EngineConfig::kSaveFileExt;
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
    for (const auto& [key, value] : flags) {
        out << key << "=" << value << "\n";
    }

    out << "[BACKLOG]\n";
    for (const auto& entry : logs) {
        out << (entry.isChoice ? 1 : 0) << "|" << entry.speaker << "|" << entry.voice << "|" << entry.text << "\n";
    }

    out.close();
    return true;
}

bool GameState::LoadGame(int slot, std::string& outScript, int& outLine, std::string& outBg, std::string& outBgm, std::vector<Models::SavedCharacter>& outCharacters) {
    std::string fileName = EngineConfig::kSaveDirectory + "/" + EngineConfig::kSaveFilePrefix + std::to_string(slot) + EngineConfig::kSaveFileExt;
    std::ifstream in(fileName);
    if (!in.is_open()) return false;

    std::string lineStr;
    int currentSection = -1;

    flags.clear();
    logs.clear();
    outCharacters.clear();

    while (std::getline(in, lineStr)) {
        if (lineStr.empty() || lineStr[0] == ';') continue;

        if (lineStr == "[STATE]") {
            currentSection = 0;
            continue;
        }
        if (lineStr == "[CHARACTERS]") {
            currentSection = 1;
            continue;
        }
        if (lineStr == "[VARIABLES]") {
            currentSection = 2;
            continue;
        }
        if (lineStr == "[BACKLOG]") {
            currentSection = 3;
            continue;
        }

        if (currentSection == 3) {
            size_t p1 = lineStr.find('|');
            size_t p2 = (p1 != std::string::npos) ? lineStr.find('|', p1 + 1) : std::string::npos;
            size_t p3 = (p2 != std::string::npos) ? lineStr.find('|', p2 + 1) : std::string::npos;
            if (p3 != std::string::npos) {
                Models::BacklogEntry entry;
                entry.isChoice = (lineStr.substr(0, p1) == "1");
                entry.speaker = lineStr.substr(p1 + 1, p2 - p1 - 1);
                entry.voice = lineStr.substr(p2 + 1, p3 - p2 - 1);
                entry.text = lineStr.substr(p3 + 1);
                logs.push_back(entry);
            }
            continue;
        }

        size_t eqPos = lineStr.find('=');
        if (eqPos != std::string::npos) {
            std::string key = lineStr.substr(0, eqPos);
            std::string val = lineStr.substr(eqPos + 1);

            if (currentSection == 0) {
                if (key == "Script")
                    outScript = val;
                else if (key == "Line")
                    outLine = std::stoi(val);
                else if (key == "BG")
                    outBg = val;
                else if (key == "BGM")
                    outBgm = val;
            }
            else if (currentSection == 1) {
                size_t commaPos = val.find(',');
                if (commaPos != std::string::npos) {
                    Models::SavedCharacter sc;
                    sc.name = key;
                    sc.diff = val.substr(0, commaPos);
                    sc.pos = std::stoi(val.substr(commaPos + 1));
                    outCharacters.push_back(sc);
                }
            }
            else if (currentSection == 2) {
                flags[key] = std::stoi(val);
            }
        }
    }
    return true;
}

void GameState::PeekSaveFile(int slot, bool& outIsEmpty, std::string& outDisplayText) {
    std::string fileName = EngineConfig::kSaveDirectory + "/" + EngineConfig::kSaveFilePrefix + std::to_string(slot) + EngineConfig::kSaveFileExt;
    std::ifstream in(fileName);
    if (!in.is_open()) {
        outIsEmpty = true;
        outDisplayText = "NO DATA";
        return;
    }

    outIsEmpty = false;
    std::string lineStr;
    std::string scriptName = "Unknown";
    std::string lineNum = "0";

    while (std::getline(in, lineStr)) {
        if (lineStr == "[CHARACTERS]" || lineStr == "[VARIABLES]") break;
        size_t eqPos = lineStr.find('=');
        if (eqPos != std::string::npos) {
            std::string key = lineStr.substr(0, eqPos);
            std::string val = lineStr.substr(eqPos + 1);
            if (key == "Script") scriptName = val;
            if (key == "Line") lineNum = val;
        }
    }

    outDisplayText = scriptName + " (Line: " + lineNum + ")";
}

}  // namespace Services
}  // namespace PrismatiX
