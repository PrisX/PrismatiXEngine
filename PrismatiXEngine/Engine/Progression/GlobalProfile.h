#pragma once

#include <set>
#include <string>
#include <unordered_map>

#include <nlohmann/json_fwd.hpp>

namespace px::progress {

class GlobalProfile {
public:
    // 既讀
    void MarkSeen(const std::string& key);
    [[nodiscard]] bool HasSeen(const std::string& key) const;

    void MarkChoiceSeen(const std::string& key);
    [[nodiscard]] bool HasChoiceSeen(const std::string& key) const;

    // 多周目
    void RegisterClear(const std::string& route);
    [[nodiscard]] int ClearCount() const { return m_clearCount; }
    [[nodiscard]] bool RouteCleared(const std::string& route) const;

    void SetPersistentVar(const std::string& name, int value);
    [[nodiscard]] int PersistentVar(const std::string& name) const;
    [[nodiscard]] const std::unordered_map<std::string, int>& PersistentVars() const { return m_vars; }

    // 劇情鎖
    void UnlockScene(const std::string& id);
    [[nodiscard]] bool SceneUnlocked(const std::string& id) const;
    void UnlockCG(const std::string& id);
    [[nodiscard]] bool CGUnlocked(const std::string& id) const;
    void UnlockMusic(const std::string& id);
    [[nodiscard]] bool MusicUnlocked(const std::string& id) const;

    [[nodiscard]] const std::set<std::string>& UnlockedScenes() const { return m_scenes; }
    [[nodiscard]] const std::set<std::string>& UnlockedCGs() const { return m_cgs; }
    [[nodiscard]] const std::set<std::string>& UnlockedMusic() const { return m_music; }

    [[nodiscard]] nlohmann::json ToJson() const;
    bool ApplyJson(const nlohmann::json& json);

private:
    std::set<std::string> m_seen;
    std::set<std::string> m_choicesSeen;
    std::set<std::string> m_clearedRoutes;
    std::set<std::string> m_scenes;
    std::set<std::string> m_cgs;
    std::set<std::string> m_music;
    std::unordered_map<std::string, int> m_vars;
    int m_clearCount = 0;
    bool m_dirty = false;
};

}  // namespace px::progress
