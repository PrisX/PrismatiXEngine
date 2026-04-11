#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "Core/Models/VNModels.h"

namespace PrismatiX::Services {

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
    const std::vector<PrismatiX::Models::BacklogEntry>& GetLogs() const;

private:
    friend class SaveManager;

    std::unordered_map<std::string, int> flags;
    std::vector<PrismatiX::Models::BacklogEntry> logs;
};

} // namespace PrismatiX::Services
