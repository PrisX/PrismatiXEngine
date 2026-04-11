#include "SaveManager.h"
#include "GameState.h"
#include "Core/EngineConfig.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace PrismatiX::Services {

SaveManager::SaveManager(GameState& gameState) : gameState(gameState) {}

bool SaveManager::SaveGame(int slot, const std::string& scriptName, int line, const std::string& bgName, const std::string& bgmName, const std::vector<PrismatiX::Models::SavedCharacter>& characters) {
    fs::create_directories(EngineConfig::kSaveDirectory);
    std::string fileName = std::string(EngineConfig::kSaveDirectory) + "/" + std::string(EngineConfig::kSaveFilePrefix) + std::to_string(slot) + std::string(EngineConfig::kSaveFileExt);
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
    for (const auto& [key, value] : gameState.flags) {
        out << key << "=" << value << "\n";
    }

    out << "[BACKLOG]\n";
    for (const auto& entry : gameState.logs) {
        out << (entry.isChoice ? 1 : 0) << "|" << entry.speaker << "|" << entry.voice << "|" << entry.text << "\n";
    }

    out.close();
    return true;
}

bool SaveManager::LoadGame(int slot, std::string& outScript, int& outLine, std::string& outBg, std::string& outBgm, std::vector<PrismatiX::Models::SavedCharacter>& outCharacters) {
    std::string fileName = std::string(EngineConfig::kSaveDirectory) + "/" + std::string(EngineConfig::kSaveFilePrefix) + std::to_string(slot) + std::string(EngineConfig::kSaveFileExt);
    std::ifstream in(fileName);
    if (!in.is_open()) return false;

    std::string lineStr;
    int currentSection = -1;

    gameState.flags.clear();
    gameState.logs.clear();
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
                PrismatiX::Models::BacklogEntry entry;
                entry.isChoice = (lineStr.substr(0, p1) == "1");
                entry.speaker = lineStr.substr(p1 + 1, p2 - p1 - 1);
                entry.voice = lineStr.substr(p2 + 1, p3 - p2 - 1);
                entry.text = lineStr.substr(p3 + 1);
                gameState.logs.push_back(entry);
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
                    PrismatiX::Models::SavedCharacter sc;
                    sc.name = key;
                    sc.diff = val.substr(0, commaPos);
                    sc.pos = std::stoi(val.substr(commaPos + 1));
                    outCharacters.push_back(sc);
                }
            }
            else if (currentSection == 2) {
                gameState.flags[key] = std::stoi(val);
            }
        }
    }
    return true;
}

void SaveManager::PeekSaveFile(int slot, bool& outIsEmpty, std::string& outDisplayText) {
    std::string fileName = std::string(EngineConfig::kSaveDirectory) + "/" + std::string(EngineConfig::kSaveFilePrefix) + std::to_string(slot) + std::string(EngineConfig::kSaveFileExt);
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

} // namespace PrismatiX::Services
