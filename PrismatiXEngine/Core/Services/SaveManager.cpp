#include "SaveManager.h"
#include "GameState.h"
#include "Core/EngineConfig.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace PrismatiX::Services {

SaveManager::SaveManager(GameState& gameState) : gameState(gameState) {}

static std::string MakeSavePath(int slot) {
    return std::string(EngineConfig::kSaveDirectory) + "/" +
           std::string(EngineConfig::kSaveFilePrefix) + std::to_string(slot) +
           std::string(EngineConfig::kSaveFileExt);
}

bool SaveManager::SaveGame(int slot, const PrismatiX::Models::SaveSnapshot& snap) {
    fs::create_directories(EngineConfig::kSaveDirectory);
    std::ofstream out(MakeSavePath(slot));
    if (!out.is_open()) return false;

    out << "[STATE]\n";
    out << "Script=" << snap.scriptName << "\n";
    out << "Line="   << snap.line       << "\n";
    out << "BG="     << snap.bgName     << "\n";
    out << "BGM="    << snap.bgmName    << "\n";

    out << "[CHARACTERS]\n";
    for (const auto& c : snap.characters)
        out << c.name << "=" << c.diff << "," << c.pos << "\n";

    out << "[VARIABLES]\n";
    out << gameState.SerializeFlags();

    out << "[BACKLOG]\n";
    out << gameState.SerializeLogs();

    return true;
}

bool SaveManager::LoadGame(int slot, PrismatiX::Models::SaveSnapshot& snap) {
    std::ifstream in(MakeSavePath(slot));
    if (!in.is_open()) return false;

    snap = {};
    std::ostringstream flagsBuf, logsBuf;
    int section = -1;
    std::string lineStr;

    while (std::getline(in, lineStr)) {
        if (!lineStr.empty() && lineStr.back() == '\r') lineStr.pop_back();
        if (lineStr.empty() || lineStr[0] == ';') continue;

        if      (lineStr == "[STATE]")      { section = 0; continue; }
        else if (lineStr == "[CHARACTERS]") { section = 1; continue; }
        else if (lineStr == "[VARIABLES]")  { section = 2; continue; }
        else if (lineStr == "[BACKLOG]")    { section = 3; continue; }

        if (section == 2) { flagsBuf << lineStr << "\n"; continue; }
        if (section == 3) { logsBuf  << lineStr << "\n"; continue; }

        auto eq = lineStr.find('=');
        if (eq == std::string::npos) continue;
        std::string key = lineStr.substr(0, eq);
        std::string val = lineStr.substr(eq + 1);

        if (section == 0) {
            if      (key == "Script") snap.scriptName = val;
            else if (key == "Line")   { try { snap.line = std::stoi(val); } catch (...) {} }
            else if (key == "BG")     snap.bgName  = val;
            else if (key == "BGM")    snap.bgmName = val;
        }
        else if (section == 1) {
            auto comma = val.find(',');
            if (comma != std::string::npos) {
                PrismatiX::Models::SavedCharacter sc;
                sc.name = key;
                sc.diff = val.substr(0, comma);
                try { sc.pos = std::stoi(val.substr(comma + 1)); } catch (...) {}
                snap.characters.push_back(sc);
            }
        }
    }

    gameState.DeserializeFlags(flagsBuf.str());
    gameState.DeserializeLogs(logsBuf.str());
    return true;
}

void SaveManager::PeekSaveFile(int slot, bool& outIsEmpty, std::string& outDisplayText) {
    std::ifstream in(MakeSavePath(slot));
    if (!in.is_open()) { outIsEmpty = true; outDisplayText = "NO DATA"; return; }

    outIsEmpty = false;
    std::string scriptName = "Unknown", lineNum = "0", lineStr;

    while (std::getline(in, lineStr)) {
        if (lineStr == "[CHARACTERS]" || lineStr == "[VARIABLES]") break;
        auto eq = lineStr.find('=');
        if (eq != std::string::npos) {
            std::string key = lineStr.substr(0, eq), val = lineStr.substr(eq + 1);
            if (key == "Script") scriptName = val;
            if (key == "Line")   lineNum    = val;
        }
    }
    outDisplayText = scriptName + " (Line: " + lineNum + ")";
}

} // namespace PrismatiX::Services
