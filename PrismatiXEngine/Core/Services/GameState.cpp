#include "GameState.h"

#include <sstream>

namespace PrismatiX::Services {

GameState::GameState() {}

void GameState::SetFlag(const std::string& name, int value) { flags[name] = value; }
void GameState::AddFlag(const std::string& name, int value) { flags[name] += value; }

int GameState::GetFlag(const std::string& name) {
    auto it = flags.find(name);
    return (it != flags.end()) ? it->second : 0;
}

const std::unordered_map<std::string, int>& GameState::GetAllFlags() const { return flags; }

bool GameState::CheckFlag(const std::string& name, const std::string& op, int compareVal) {
    int v = GetFlag(name);
    if (op == "==") return v == compareVal;
    if (op == "!=") return v != compareVal;
    if (op == ">") return v > compareVal;
    if (op == "<") return v < compareVal;
    if (op == ">=") return v >= compareVal;
    if (op == "<=") return v <= compareVal;
    return false;
}

void GameState::ClearFlags() { flags.clear(); }

void GameState::AddLog(const std::string& speaker, const std::string& text, const std::string& voice) {
    if (text.empty()) return;
    logs.push_back({ speaker, text, voice, false });
}

void GameState::AddChoiceLog(const std::string& text) { logs.push_back({ "", text, "", true }); }
void GameState::ClearLogs() { logs.clear(); }
const std::vector<PrismatiX::Models::BacklogEntry>& GameState::GetLogs() const { return logs; }

std::string GameState::SerializeFlags() const {
    std::ostringstream ss;
    for (const auto& [k, v] : flags) ss << k << "=" << v << "\n";
    return ss.str();
}

// 分隔用的神奇 SOH
static constexpr char kLogSep = '\x01';

std::string GameState::SerializeLogs() const {
    std::ostringstream ss;
    for (const auto& e : logs) {
        ss << (e.isChoice ? 1 : 0) << kLogSep << e.speaker << kLogSep << e.voice << kLogSep << e.text << "\n";
    }
    return ss.str();
}

void GameState::DeserializeFlags(const std::string& data) {
    flags.clear();
    std::istringstream ss(data);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        try {
            flags[key] = std::stoi(line.substr(eq + 1));
        } catch (...) {
        }
    }
}

void GameState::DeserializeLogs(const std::string& data) {
    logs.clear();
    std::istringstream ss(data);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        // 格式：isChoice\x01speaker\x01voice\x01text
        auto s1 = line.find(kLogSep);
        auto s2 = (s1 != std::string::npos) ? line.find(kLogSep, s1 + 1) : std::string::npos;
        auto s3 = (s2 != std::string::npos) ? line.find(kLogSep, s2 + 1) : std::string::npos;
        if (s3 == std::string::npos) continue;

        PrismatiX::Models::BacklogEntry e;
        e.isChoice = (line.substr(0, s1) == "1");
        e.speaker = line.substr(s1 + 1, s2 - s1 - 1);
        e.voice = line.substr(s2 + 1, s3 - s2 - 1);
        e.text = line.substr(s3 + 1);
        logs.push_back(e);
    }
}

}  // namespace PrismatiX::Services
