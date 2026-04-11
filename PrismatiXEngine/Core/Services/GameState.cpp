#include "GameState.h"

#include <Core/EngineConfig.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace PrismatiX::Services {

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

const std::vector<PrismatiX::Models::BacklogEntry>& GameState::GetLogs() const { return logs; }

} // namespace PrismatiX::Services
